#!/usr/bin/env python3
"""The glassly example-miniapp "shapes" test suite, run straight against the G1 over BLE.

Every case is the same data as the miniapp's src/shared/shapesSuite.ts (ported
from g2flash/demos/shapes-suite.ts); what the phone does between render() and
the radio (anchor resolution, validate / clamp / budget, frame diffing, the
retained-object encoder) lives in cfw/scene.py, so the verdicts mean the same
thing here as in the tester page.

What differs from the phone: default-font text is not wrapped or measured (the
glasses' font engine wraps and clips it inside its box), so a truncated text
box never marks a frame degraded here. Text has no grey level on the G1.
Images are not supported.

Pacing: a step's frame is sent `holdMs` after the previous step's frame was
sent (never before that frame was acked), so each frame is on the glasses for
about its hold and a later frame never waits for an earlier frame's
transitions to finish. Only a case's last frame waits for its transitions
before the hold and blank.

    tools/shapes_suite.py --left UUID --right UUID    # every case, both temples
    tools/shapes_suite.py --left UUID rect animation   # by group
    tools/shapes_suite.py --left UUID anim-glide       # by id
    tools/shapes_suite.py --list                       # ids and groups, no connection
    tools/shapes_suite.py --self-test                  # encoder vectors, byte for byte
    tools/shapes_suite.py --dump FILE                  # every message + wait for the host replay
    tools/shapes_suite.py --dry-run                    # host pipeline only (also G1_DRY_RUN=1)
    tools/shapes_suite.py --left UUID --trace          # per-frame op counts and timings
    tools/shapes_suite.py --left UUID --hold-scale 0.5 --out results.json

Needs G1CFW/38 or later (kind-4 shape objects and tween animation).
"""
import argparse
import asyncio
from contextlib import AsyncExitStack
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cfw.scene import (CANVAS, ARC, CIRCLE, CIRCLE_FILL, LINE, PIE, RECT, RECT_FILL, ASSET_CLEAR, OBJ_CONTROL, OBJ_HIDE,  # noqa: E402
                       RESET_RECORDS, G1Scene, anim_entry, crc16, diff_scene, easing_curve, messages, obj_control_payload,
                       obj_hide_payload, process_scene, record, resolve_anchors, stream_packets, svg_path, transition_frames,
                       evaluate_result, jround)

REQUIRED_REVISION = 38
ACK_TIMEOUT = 8

# ============================================================================
# Suite data: src/shared/shapesSuite.ts, verbatim where Python allows
# ============================================================================

EASE = {'durationMs': 400, 'easing': 'ease-in-out'}
HOLD = 700
OK = {'status': 'displayed', 'degraded': False, 'dropped': []}


def unsupported(canvas, ids):
    dropped = [id_ for id_, type_ in ids if type_ != 'rect' and type_ not in canvas['shapes']]
    return {'status': 'displayed', 'degraded': bool(dropped), 'dropped': dropped}


def single(type_, id_):
    return lambda c: unsupported(c, [(id_, type_)])


def frame(canvas, *elements):
    return [{'elements': [border(canvas), *elements], 'holdMs': HOLD}]


def border(canvas):
    return {'type': 'rect', 'id': 'suite-border', 'box': {'x': 0, 'y': 0, 'w': canvas['width'], 'h': canvas['height']}, 'style': {'border': 1}}


def center(canvas, w, h):
    return {'x': (canvas['width'] - w) // 2, 'y': (canvas['height'] - h) // 2, 'w': w, 'h': h}


def case(id_, group, title, description, steps, expect, blank_after=False):
    return {'id': id_, 'group': group, 'title': title, 'description': description, 'steps': steps, 'expect': expect, 'blankAfter': blank_after}


def rect_gray_ramp_steps(c):
    w = (c['width'] - 32) // 16
    bars = [{'type': 'rect', 'id': f'bar-{i}', 'box': {'x': 16 + i * w, 'y': 40, 'w': w - 2, 'h': c['height'] - 80}, 'style': {'fill': True, 'color': i}}
            for i in range(16)]
    return frame(c, *bars)


def rect_gray_ramp_expect(c):
    ids = ['suite-border'] + [f'bar-{i}' for i in range(16)]
    dropped = ids[c['maxTextElements']:]
    return {'status': 'displayed', 'degraded': bool(dropped), 'dropped': dropped}


def circle_inscribed_steps(c):
    box = center(c, 240, 80)
    return frame(c, {'type': 'rect', 'id': 'box', 'box': box, 'style': {'border': 1, 'color': 6}}, {'type': 'circle', 'id': 'c', 'box': box, 'style': {'fill': True}})


def line_wide_steps(c):
    return frame(c, *[{'type': 'line', 'id': f'l{width}', 'points': [{'x': 40, 'y': 60 + i * 60}, {'x': c['width'] - 40, 'y': 60 + i * 60}], 'style': {'width': width}}
                      for i, width in enumerate([2, 6, 12])])


def triangle_steps(c):
    y0 = c['height'] - 40
    y1 = 40
    return frame(
        c,
        {'type': 'triangle', 'id': 't-out', 'points': [{'x': 40, 'y': y0}, {'x': 220, 'y': y0}, {'x': 130, 'y': y1}], 'style': {'border': 2}},
        {'type': 'triangle', 'id': 't-fill', 'points': [{'x': c['width'] - 220, 'y': y0}, {'x': c['width'] - 40, 'y': y0}, {'x': c['width'] - 130, 'y': y1}], 'style': {'fill': True, 'color': 10}},
    )


def quad_steps(c):
    return frame(
        c,
        {'type': 'quad', 'id': 'q-out', 'points': [{'x': 60, 'y': 60}, {'x': 230, 'y': 80}, {'x': 210, 'y': c['height'] - 60}, {'x': 40, 'y': c['height'] - 80}], 'style': {'border': 2}},
        {'type': 'quad', 'id': 'q-fill', 'points': [{'x': c['width'] - 230, 'y': 60}, {'x': c['width'] - 60, 'y': 80}, {'x': c['width'] - 40, 'y': c['height'] - 60}, {'x': c['width'] - 210, 'y': c['height'] - 80}], 'style': {'fill': True}},
    )


def bezier_cubic_steps(c):
    return frame(c, {
        'type': 'bezier', 'id': 'b',
        'points': [
            {'x': 40, 'y': c['height'] // 2},
            {'x': c['width'] // 3, 'y': 20},
            {'x': (2 * c['width']) // 3, 'y': c['height'] - 20},
            {'x': c['width'] - 40, 'y': c['height'] // 2},
        ],
        'style': {'width': 4, 'color': 13},
    })


def scene_painters_order_steps(c):
    return frame(
        c,
        {'type': 'rect', 'id': 'bg', 'box': center(c, 300, 140), 'style': {'fill': True, 'color': 5}},
        {'type': 'text', 'id': 'label', 'box': center(c, 280, 40), 'text': 'shapes on top'},
        {'type': 'circle', 'id': 'dot', 'box': {'x': center(c, 300, 140)['x'] + 240, 'y': center(c, 300, 140)['y'] + 20, 'w': 60, 'h': 60}, 'style': {'fill': True}},
    )


def scene_anchored_steps(c):
    return frame(
        c,
        {'type': 'circle', 'id': 'br', 'box': {'x': 8, 'y': 8, 'w': 60, 'h': 60, 'anchor': 'bottom-right'}, 'style': {'fill': True}},
        {'type': 'rect', 'id': 'tr', 'box': {'x': 8, 'y': 8, 'w': 120, 'h': 40, 'anchor': 'top-right'}, 'style': {'border': 2, 'radius': 6}},
    )


def limits_budget_steps(c):
    # Small enough discs that budget + 4 of them fit on the canvas: the
    # budget, not the clamp, must be what drops the tail.
    n = c['maxTextElements'] + 4
    pitch = 24
    cols = max(1, (c['width'] - 8) // pitch)
    discs = [{'type': 'circle', 'id': f'd{i}', 'box': {'x': 8 + (i % cols) * pitch, 'y': 8 + (i // cols) * pitch, 'w': 20, 'h': 20}, 'style': {'fill': True, 'color': 4 + (i % 12)}}
             for i in range(n)]
    return [{'elements': discs, 'holdMs': HOLD}]


def limits_budget_expect(c):
    n = c['maxTextElements'] + 4
    ids = [f'd{i}' for i in range(n)]
    dropped = ids[c['maxTextElements']:] if 'circle' in c['shapes'] else ids
    return {'status': 'displayed', 'degraded': True, 'dropped': dropped}


def limits_bad_points_steps(c):
    return frame(
        c,
        {'type': 'triangle', 'id': 'bad-tri', 'points': [{'x': 10, 'y': 10}, {'x': 100, 'y': 10}]},
        {'type': 'bezier', 'id': 'bad-bez', 'points': [{'x': 10, 'y': 10}, {'x': 20, 'y': 20}, {'x': 30, 'y': 30}, {'x': 40, 'y': 40}, {'x': 50, 'y': 50}]},
        {'type': 'line', 'id': 'good', 'points': [{'x': 40, 'y': c['height'] // 2}, {'x': c['width'] - 40, 'y': c['height'] // 2}], 'style': {'width': 2}},
    )


def limits_bad_points_expect(c):
    dropped = ['bad-tri', 'bad-bez'] + ([] if 'line' in c['shapes'] else ['good'])
    return {'status': 'displayed', 'degraded': True, 'dropped': dropped}


def limits_unsupported_steps(c):
    y = c['height'] // 2
    return frame(
        c,
        {'type': 'circle', 'id': 'g-circle', 'box': {'x': 20, 'y': y - 30, 'w': 60, 'h': 60}, 'style': {'fill': True}},
        {'type': 'arc', 'id': 'g-arc', 'box': {'x': 100, 'y': y - 30, 'w': 60, 'h': 60}, 'startDeg': 0, 'endDeg': 270, 'style': {'width': 4}},
        {'type': 'pie', 'id': 'g-pie', 'box': {'x': 180, 'y': y - 30, 'w': 60, 'h': 60}, 'startDeg': 0, 'endDeg': 120},
        {'type': 'line', 'id': 'g-line', 'points': [{'x': 260, 'y': y - 30}, {'x': 320, 'y': y + 30}]},
        {'type': 'triangle', 'id': 'g-tri', 'points': [{'x': 340, 'y': y + 30}, {'x': 400, 'y': y + 30}, {'x': 370, 'y': y - 30}]},
        {'type': 'quad', 'id': 'g-quad', 'points': [{'x': 420, 'y': y - 30}, {'x': 480, 'y': y - 30}, {'x': 470, 'y': y + 30}, {'x': 430, 'y': y + 30}]},
        {'type': 'bezier', 'id': 'g-bez', 'points': [{'x': 500, 'y': y + 30}, {'x': 530, 'y': y - 40}, {'x': 560, 'y': y + 30}]},
    )


LIMITS_UNSUPPORTED_IDS = [('g-circle', 'circle'), ('g-arc', 'arc'), ('g-pie', 'pie'), ('g-line', 'line'), ('g-tri', 'triangle'), ('g-quad', 'quad'), ('g-bez', 'bezier')]


def anim_glide_steps(c):
    y = c['height'] // 2 - 25

    def at(x):
        return {'type': 'circle', 'id': 'ball', 'box': {'x': x, 'y': y, 'w': 50, 'h': 50}, 'style': {'fill': True}, 'transition': EASE}
    return [
        {'elements': [border(c), at(20)], 'holdMs': HOLD},
        {'elements': [border(c), at(c['width'] - 70)], 'holdMs': HOLD},
        {'elements': [border(c), at(20)], 'holdMs': HOLD},
    ]


def anim_resize_steps(c):
    track = center(c, c['width'] - 80, 24)

    def bar(w):
        return [
            border(c),
            {'type': 'rect', 'id': 'track', 'box': track, 'style': {'border': 1, 'radius': 12, 'color': 7}},
            {'type': 'rect', 'id': 'bar', 'box': {'x': track['x'], 'y': track['y'], 'w': max(2, w), 'h': 24}, 'style': {'fill': True, 'radius': 12}, 'transition': {'durationMs': 900, 'easing': 'linear'}},
        ]
    return [
        {'elements': bar(2), 'holdMs': 300},
        {'elements': bar(track['w']), 'holdMs': 1200},
    ]


def anim_arc_sweep_steps(c):
    def at(end):
        return [
            border(c),
            {'type': 'arc', 'id': 'gauge', 'box': {'x': 60, 'y': center(c, 140, 140)['y'], 'w': 140, 'h': 140}, 'startDeg': 135, 'endDeg': 135 + end, 'style': {'width': 10}, 'transition': {'durationMs': 800, 'easing': 'ease-out'}},
            {'type': 'pie', 'id': 'wedge', 'box': {'x': c['width'] - 200, 'y': center(c, 140, 140)['y'], 'w': 140, 'h': 140}, 'startDeg': -90, 'endDeg': -90 + end, 'style': {'color': 10}, 'transition': {'durationMs': 800, 'easing': 'ease-out'}},
        ]
    return [
        {'elements': at(1), 'holdMs': 300},
        {'elements': at(270), 'holdMs': 1100},
    ]


def anim_color_stroke_steps(c):
    def ring(color, stroke):
        return {'type': 'circle', 'id': 'ring', 'box': center(c, 140, 140), 'style': {'border': stroke, 'color': color}, 'transition': {'durationMs': 700}}
    return [
        {'elements': [border(c), ring(2, 1)], 'holdMs': 300},
        {'elements': [border(c), ring(15, 12)], 'holdMs': 1000},
    ]


def anim_points_steps(c):
    def tri(apex_x):
        return [
            border(c),
            {'type': 'triangle', 'id': 'tri', 'points': [{'x': 60, 'y': c['height'] - 40}, {'x': c['width'] - 60, 'y': c['height'] - 40}, {'x': apex_x, 'y': 40}], 'style': {'fill': True, 'color': 12}, 'transition': {'durationMs': 600, 'easing': 'ease-in-out'}},
        ]
    return [
        {'elements': tri(80), 'holdMs': 300},
        {'elements': tri(c['width'] - 80), 'holdMs': 900},
        {'elements': tri(80), 'holdMs': 900},
    ]


def anim_compose_steps(c):
    def at(x, y):
        return [
            border(c),
            {'type': 'circle', 'id': 'ball', 'box': {'x': x, 'y': y, 'w': 40, 'h': 40}, 'style': {'fill': True}, 'transition': {'durationMs': 800}},
        ]
    return [
        {'elements': at(20, 20), 'holdMs': 200},
        {'elements': at(c['width'] - 60, 20), 'holdMs': 250},
        {'elements': at(c['width'] - 60, c['height'] - 60), 'holdMs': 1000},
    ]


def anim_no_transition_steps(c):
    y = c['height'] // 2 - 25

    def at(x):
        return [border(c), {'type': 'circle', 'id': 'ball', 'box': {'x': x, 'y': y, 'w': 50, 'h': 50}, 'style': {'fill': True}}]
    return [
        {'elements': at(20), 'holdMs': HOLD},
        {'elements': at(c['width'] - 70), 'holdMs': HOLD},
    ]


def anim_content_switch_steps(c):
    def at(x, label):
        return [
            border(c),
            {'type': 'text', 'id': 't', 'box': {'x': 20, 'y': 20, 'w': 300, 'h': 40}, 'text': label, 'transition': EASE},
            {'type': 'circle', 'id': 'ball', 'box': {'x': x, 'y': 100, 'w': 50, 'h': 50}, 'style': {'fill': True}, 'transition': EASE},
        ]
    return [
        {'elements': at(20, 'before'), 'holdMs': HOLD},
        {'elements': at(c['width'] - 70, 'after'), 'holdMs': HOLD},
    ]


def text_counter_steps(c):
    return [{
        'elements': [
            border(c),
            {'type': 'text', 'id': 'label', 'box': {'x': 20, 'y': 20, 'w': 300, 'h': 30}, 'text': 'counter at 10 fps'},
            {'type': 'text', 'id': 'count', 'box': center(c, 200, 40), 'text': str(i)},
        ],
        'holdMs': 100,
    } for i in range(30)]


def text_typewriter_steps(c):
    sentence = 'The glasses draw this text themselves.'
    return [{
        'elements': [border(c), {'type': 'text', 'id': 'typed', 'box': {'x': 20, 'y': 40, 'w': c['width'] - 40, 'h': 60}, 'text': sentence[:i + 1]}],
        'holdMs': 60,
    } for i in range(len(sentence))]


def text_slide_steps(c):
    def at(x):
        return [border(c), {'type': 'text', 'id': 'slider', 'box': {'x': x, 'y': 60, 'w': 180, 'h': 30}, 'text': 'sliding text', 'transition': EASE}]
    return [
        {'elements': at(20), 'holdMs': HOLD},
        {'elements': at(c['width'] - 200), 'holdMs': HOLD},
        {'elements': at(20), 'holdMs': HOLD},
    ]


def text_captions_steps(c):
    lines = ['first line arrives', 'second line arrives', 'third line arrives', 'fourth line arrives', 'fifth line arrives', 'sixth line arrives']
    rows = 3
    pitch = 27
    top = c['height'] - 20 - rows * pitch
    steps = []
    for step in range(len(lines)):
        visible = lines[max(0, step - rows + 1):step + 1]
        first = step - len(visible) + 1
        steps.append({
            'elements': [
                border(c),
                *[{'type': 'text', 'id': f'cap-{first + i}', 'box': {'x': 20, 'y': top + i * pitch, 'w': c['width'] - 40, 'h': pitch}, 'text': text,
                   'transition': {'durationMs': 250, 'easing': 'ease-out'}} for i, text in enumerate(visible)],
            ],
            'holdMs': 500,
        })
    return steps


def text_fade_steps(c):
    def at(color):
        return [
            border(c),
            {'type': 'text', 'id': 'fader', 'box': center(c, 240, 30), 'text': 'fading in and out', 'style': {'color': color}, 'transition': {'durationMs': 600, 'easing': 'linear'}},
        ]
    return [
        {'elements': at(0), 'holdMs': 200},
        {'elements': at(15), 'holdMs': 900},
        {'elements': at(0), 'holdMs': 900},
    ]


def js_tenths(i):
    """String(i / 10) as JavaScript prints it."""
    return str(i // 10) if i % 10 == 0 else str(i / 10)


def text_clock_steps(c):
    return [{
        'elements': [
            border(c),
            {'type': 'text', 'id': 'clock', 'box': {'x': 20, 'y': 20, 'w': 200, 'h': 30}, 'text': f'00:{js_tenths(i).rjust(4, "0")}'},
            {'type': 'arc', 'id': 'spinner', 'box': {'x': c['width'] - 80, 'y': 20, 'w': 50, 'h': 50}, 'startDeg': i * 36, 'endDeg': i * 36 + 270, 'style': {'width': 4},
             'transition': {'durationMs': 100, 'easing': 'linear'}},
        ],
        'holdMs': 100,
    } for i in range(30)]


SHAPE_CASES = [
    # -- rect --
    case('rect-outline', 'rect', 'rect outline + radius', 'A 2 px rounded outline in the middle of the screen.',
         lambda c: frame(c, {'type': 'rect', 'id': 'r', 'box': center(c, 200, 100), 'style': {'border': 2, 'radius': 12}}),
         lambda c: OK),
    case('rect-fill', 'rect', 'rect fill + border', 'A solid mid-gray block with a bright 3 px border.',
         lambda c: frame(c, {'type': 'rect', 'id': 'r', 'box': center(c, 200, 100), 'style': {'fill': True, 'color': 8, 'border': 3, 'radius': 8}}),
         lambda c: OK),
    case('rect-gray-ramp', 'rect', '16 gray levels', 'Sixteen filled bars from black (0) to brightest (15), left to right.',
         rect_gray_ramp_steps, rect_gray_ramp_expect),

    # -- circle --
    case('circle-outline', 'circle', 'circle outline', 'A thin ring centered on screen.',
         lambda c: frame(c, {'type': 'circle', 'id': 'c', 'box': center(c, 120, 120), 'style': {'border': 1}}),
         single('circle', 'c')),
    case('circle-fill-ring', 'circle', 'disc + thick ring', 'A filled disc on the left, an 8 px ring on the right.',
         lambda c: frame(
             c,
             {'type': 'circle', 'id': 'disc', 'box': {'x': 60, 'y': center(c, 100, 100)['y'], 'w': 100, 'h': 100}, 'style': {'fill': True}},
             {'type': 'circle', 'id': 'ring', 'box': {'x': c['width'] - 160, 'y': center(c, 100, 100)['y'], 'w': 100, 'h': 100}, 'style': {'border': 8, 'color': 11}},
         ),
         lambda c: unsupported(c, [('disc', 'circle'), ('ring', 'circle')])),
    case('circle-inscribed', 'circle', 'inscribed in a wide box', 'The circle uses the shorter side: a disc centered in the outlined box, never an ellipse.',
         circle_inscribed_steps, single('circle', 'c')),

    # -- arc + pie --
    case('arc-quarter', 'arc', 'quarter arc', 'A 4 px arc from the top (−90°) clockwise to the right (0°).',
         lambda c: frame(c, {'type': 'arc', 'id': 'a', 'box': center(c, 140, 140), 'startDeg': -90, 'endDeg': 0, 'style': {'width': 4}}),
         single('arc', 'a')),
    case('arc-gauge', 'arc', 'gauge arc', 'A thick 270° arc open at the bottom, like a gauge dial.',
         lambda c: frame(c, {'type': 'arc', 'id': 'a', 'box': center(c, 160, 160), 'startDeg': 135, 'endDeg': 405, 'style': {'width': 10, 'color': 12}}),
         single('arc', 'a')),
    case('pie-sector', 'arc', 'pie sector', 'A filled wedge from the top clockwise to the lower right (225° sweep).',
         lambda c: frame(c, {'type': 'pie', 'id': 'p', 'box': center(c, 140, 140), 'startDeg': -90, 'endDeg': 135}),
         single('pie', 'p')),
    case('pie-full', 'arc', 'full pie', 'A 360° pie equals a filled disc.',
         lambda c: frame(c, {'type': 'pie', 'id': 'p', 'box': center(c, 100, 100), 'startDeg': 0, 'endDeg': 360, 'style': {'color': 9}}),
         single('pie', 'p')),

    # -- line --
    case('line-hairline', 'line', 'hairline', 'A 1 px diagonal from top-left to bottom-right of the screen.',
         lambda c: frame(c, {'type': 'line', 'id': 'l', 'points': [{'x': 8, 'y': 8}, {'x': c['width'] - 9, 'y': c['height'] - 9}]}),
         single('line', 'l')),
    case('line-wide', 'line', 'wide strokes', 'Three horizontal lines 2, 6 and 12 px thick.',
         line_wide_steps, lambda c: unsupported(c, [('l2', 'line'), ('l6', 'line'), ('l12', 'line')])),
    case('line-clipped', 'line', 'clipped by the device', 'A line that runs off the right edge is drawn up to the edge and reported as degraded, not dropped.',
         lambda c: frame(c, {'type': 'line', 'id': 'l', 'points': [{'x': 40, 'y': c['height'] - 40}, {'x': c['width'] + 200, 'y': 40}], 'style': {'width': 3}}),
         lambda c: {**unsupported(c, [('l', 'line')]), 'degraded': True}),

    # -- polygons --
    case('triangle', 'polygon', 'triangle outline + fill', 'An outlined triangle on the left and a filled one on the right.',
         triangle_steps, lambda c: unsupported(c, [('t-out', 'triangle'), ('t-fill', 'triangle')])),
    case('quad', 'polygon', 'quad outline + fill', 'A skewed outlined quad on the left and a filled one on the right.',
         quad_steps, lambda c: unsupported(c, [('q-out', 'quad'), ('q-fill', 'quad')])),

    # -- bezier --
    case('bezier-quadratic', 'bezier', 'quadratic bezier', 'A single arch pulled up by one control point.',
         lambda c: frame(c, {'type': 'bezier', 'id': 'b', 'points': [{'x': 40, 'y': c['height'] - 40}, {'x': c['width'] // 2, 'y': -40}, {'x': c['width'] - 40, 'y': c['height'] - 40}], 'style': {'width': 3}}),
         lambda c: {**unsupported(c, [('b', 'bezier')]), 'degraded': True}),
    case('bezier-cubic', 'bezier', 'cubic bezier', 'An S-curve from left to right.',
         bezier_cubic_steps, single('bezier', 'b')),

    # -- scene composition --
    case('scene-painters-order', 'scene', "painter's order", 'Filled rect, then text on top of it, then a bright disc over both: later elements paint over earlier ones.',
         scene_painters_order_steps, single('circle', 'dot')),
    case('scene-anchored', 'scene', 'anchored boxes', 'A circle flush in the bottom-right corner and a rect flush top-right, on every canvas size.',
         scene_anchored_steps, single('circle', 'br')),
    case('scene-clear', 'scene', 'render([]) clears', 'The screen goes blank.',
         lambda c: [{'elements': [], 'holdMs': 200}], lambda c: OK),

    # -- limits + validation --
    case('limits-budget', 'limits', 'element budget', 'More discs than the budget: the tail is dropped and reported, the rest draws.',
         limits_budget_steps, limits_budget_expect),
    case('limits-bad-points', 'limits', 'invalid point counts', 'A 2-point triangle and a 5-point bezier are rejected; the valid line still draws.',
         limits_bad_points_steps, limits_bad_points_expect),
    case('limits-unsupported', 'limits', 'capability gate', 'Every shape type at once. Types the device lists draw; the others are dropped and reported, in order.',
         limits_unsupported_steps, lambda c: unsupported(c, LIMITS_UNSUPPORTED_IDS)),

    # -- animation --
    case('anim-glide', 'animation', 'glide (move)', 'A disc eases from the left edge to the right edge and back. Without animation it jumps.',
         anim_glide_steps, single('circle', 'ball')),
    case('anim-resize', 'animation', 'tween (resize)', 'A progress bar grows from empty to full inside its track.',
         anim_resize_steps, lambda c: OK),
    case('anim-arc-sweep', 'animation', 'tween (angles)', 'A gauge arc sweeps from 0° to 270° and a pie sector opens with it.',
         anim_arc_sweep_steps, lambda c: unsupported(c, [('gauge', 'arc'), ('wedge', 'pie')])),
    case('anim-color-stroke', 'animation', 'tween (color + stroke)', 'A ring brightens from dim to full and thickens from 1 px to 12 px.',
         anim_color_stroke_steps, single('circle', 'ring')),
    case('anim-points', 'animation', 'tween (points)', 'A triangle morphs as its apex slides from left to right.',
         anim_points_steps, single('triangle', 'tri')),
    case('anim-compose', 'animation', 'compose in-flight glides', 'A second move lands while the first is still running; the disc ends at the final target without a snap.',
         anim_compose_steps, single('circle', 'ball')),
    case('anim-no-transition', 'animation', 'control: no transition', 'The same move without a transition jumps instantly on every device.',
         anim_no_transition_steps, single('circle', 'ball')),
    case('anim-content-switch', 'animation', 'content changes never animate', 'Text with a transition changes wording: it switches immediately while a disc beside it glides.',
         anim_content_switch_steps, single('circle', 'ball')),

    # -- text: fast updates + animation --
    case('text-counter', 'text', 'counter at 10 fps', 'A number counts 0 to 29 at ten updates per second; every frame should land without tearing.',
         text_counter_steps, lambda c: OK),
    case('text-typewriter', 'text', 'typewriter', 'A sentence appears one character at a time, 60 ms apart.',
         text_typewriter_steps, lambda c: OK),
    case('text-slide', 'text', 'slide (glide)', 'A label glides from the left edge to the right edge and back. Without animation it jumps.',
         text_slide_steps, lambda c: OK),
    case('text-captions', 'text', 'caption scroll', 'New caption lines arrive at the bottom while older lines glide up and the oldest disappears.',
         text_captions_steps, lambda c: OK),
    case('text-fade', 'text', 'fade (color tween)', 'A label fades in from black to full brightness, then back out.',
         text_fade_steps, lambda c: OK),
    case('text-clock', 'text', 'tenths clock', 'A clock ticks in tenths of a second for three seconds next to a spinning arc.',
         text_clock_steps, single('arc', 'spinner')),
]

# -- stages: the g2flash shapes-demo, stage for stage, through render() --

STAGE_EASE = {
    'linear': {'durationMs': 660, 'easing': 'linear'},
    'inOut': {'durationMs': 1500, 'easing': 'ease-in-out'},
}


def budgeted(canvas, elements):
    """What the host drops from `elements` on this device: unsupported shapes, then text/rect past the budget, in element order."""
    dropped = []
    pool = canvas['maxTextElements']
    for el in elements:
        id_ = el.get('id') or ''
        if el['type'] == 'image':
            continue
        if el['type'] != 'text' and el['type'] != 'rect' and el['type'] not in canvas['shapes']:
            dropped.append(id_)
            continue
        if pool <= 0:
            dropped.append(id_)
            continue
        pool -= 1
    return {'status': 'displayed', 'degraded': bool(dropped), 'dropped': dropped}


def clipped(canvas, elements):
    """True when a box runs past the canvas: the host clamps it and reports the frame degraded.
    The stages were laid out for the G2's 288 rows; on 200 rows stage 2's last grey row is cut."""
    return any(el.get('box') and (el['box']['x'] + el['box']['w'] > canvas['width'] or el['box']['y'] + el['box']['h'] > canvas['height'])
               for el in elements)


def stage_label(canvas, n, name):
    return [
        border(canvas),
        {'type': 'text', 'id': 'stage-label', 'box': {'x': 12, 'y': 6, 'w': canvas['width'] - 24, 'h': 24}, 'text': f'stage {n}: {name}', 'style': {'color': 7}},
    ]


def stage_shapes(c):
    s = min(c['width'] / 576, c['height'] / 288)

    def px(v):
        return jround(v * s)
    return [
        {'type': 'line', 'id': 's-line', 'points': [{'x': px(30), 'y': px(50)}, {'x': px(540), 'y': px(50)}], 'style': {'width': 3, 'color': 12}},
        {'type': 'circle', 'id': 's-disc', 'box': {'x': px(30), 'y': px(70), 'w': px(90), 'h': px(90)}, 'style': {'fill': True}},
        {'type': 'circle', 'id': 's-ring', 'box': {'x': px(140), 'y': px(70), 'w': px(90), 'h': px(90)}, 'style': {'border': 5, 'color': 12}},
        {'type': 'rect', 'id': 's-rect', 'box': {'x': px(250), 'y': px(70), 'w': px(120), 'h': px(90)}, 'style': {'fill': True, 'color': 10, 'radius': 14}},
        {'type': 'triangle', 'id': 's-tri', 'points': [{'x': px(30), 'y': px(260)}, {'x': px(120), 'y': px(260)}, {'x': px(75), 'y': px(185)}], 'style': {'fill': True}},
        {'type': 'quad', 'id': 's-quad', 'points': [{'x': px(150), 'y': px(185)}, {'x': px(260), 'y': px(192)}, {'x': px(252), 'y': px(262)}, {'x': px(158), 'y': px(255)}], 'style': {'border': 2}},
        {'type': 'bezier', 'id': 's-bez', 'points': [{'x': px(290), 'y': px(250)}, {'x': px(330), 'y': px(180)}, {'x': px(380), 'y': px(280)}, {'x': px(440), 'y': px(230)}], 'style': {'width': 3}},
        {'type': 'arc', 'id': 's-arc', 'box': {'x': px(400), 'y': px(70), 'w': px(100), 'h': px(100)}, 'startDeg': 180, 'endDeg': 360, 'style': {'width': 4}},
        {'type': 'pie', 'id': 's-pie', 'box': {'x': px(425), 'y': px(95), 'w': px(50), 'h': px(50)}, 'startDeg': -90, 'endDeg': 180, 'style': {'color': 9}},
    ]


STAGE_SHAPE_IDS = [
    ('s-line', 'line'), ('s-disc', 'circle'), ('s-ring', 'circle'), ('s-rect', 'rect'), ('s-tri', 'triangle'),
    ('s-quad', 'quad'), ('s-bez', 'bezier'), ('s-arc', 'arc'), ('s-pie', 'pie'),
]


def stage_2_steps(c):
    lines = ['The glasses draw this text themselves.', 'Each line is one slot with its bytes inline.', 'No bitmaps and no re-sends.']
    w = c['width'] - 40
    return [{
        'elements': [
            *stage_label(c, 2, 'text lines'),
            *[{'type': 'text', 'id': f'line-{i}', 'box': {'x': 20, 'y': 34 + i * 26, 'w': w, 'h': 26}, 'text': text} for i, text in enumerate(lines)],
            *[{'type': 'text', 'id': f'gray-{color}', 'box': {'x': 20, 'y': 114 + i * 20, 'w': 200, 'h': 20}, 'text': f'gray level {color}', 'style': {'color': color}}
              for i, color in enumerate([15, 12, 9, 6, 3])],
            {'type': 'text', 'id': 'clipped', 'box': {'x': 240, 'y': 116, 'w': min(220, c['width'] - 260), 'h': 24}, 'text': 'clipped at the box edge: the rest of this sentence is cut', 'style': {'overflow': 'clip'}},
            {'type': 'rect', 'id': 'clip-box', 'box': {'x': 238, 'y': 114, 'w': min(224, c['width'] - 256), 'h': 28}, 'style': {'border': 1, 'color': 5}},
            {'type': 'text', 'id': 'utf8', 'box': {'x': 240, 'y': 150, 'w': c['width'] - 260, 'h': 24}, 'text': 'UTF-8: café, naïve, 日本語, ∑'},
        ],
        'holdMs': 3500,
    }]


def stage_2_expect(c):
    elements = stage_2_steps(c)[0]['elements']
    expectation = budgeted(c, elements)
    return {**expectation, 'degraded': expectation['degraded'] or clipped(c, elements)}


def stage_3_steps(c):
    def row(id_, y, text):
        return {'type': 'text', 'id': id_, 'box': {'x': 200, 'y': y, 'w': c['width'] - 220, 'h': 26}, 'text': text}

    def base(count, typed, clock):
        return [
            *stage_label(c, 3, 'fast text updates'),
            {'type': 'text', 'id': 'l-count', 'box': {'x': 20, 'y': 50, 'w': 170, 'h': 26}, 'text': 'counter', 'style': {'color': 9}},
            row('count', 50, count),
            {'type': 'text', 'id': 'l-typed', 'box': {'x': 20, 'y': 90, 'w': 170, 'h': 26}, 'text': 'typewriter', 'style': {'color': 9}},
            row('typed', 90, typed),
            {'type': 'text', 'id': 'l-clock', 'box': {'x': 20, 'y': 130, 'w': 170, 'h': 26}, 'text': 'tenths clock', 'style': {'color': 9}},
            row('clock', 130, clock),
        ]
    sentence = 'typed one character per render'
    steps = []
    for i in range(40):
        steps.append({'elements': base(str(i + 1), ' ', '00.0'), 'holdMs': 0})
    for i in range(len(sentence)):
        steps.append({'elements': base('40', sentence[:i + 1], '00.0'), 'holdMs': 0})
    for i in range(40):
        steps.append({'elements': base('40', sentence, f'{i // 10:02d}.{i % 10}'), 'holdMs': 0})
    steps[-1]['holdMs'] = 1500
    return steps


def stage_4_steps(c):
    steps = []
    slide = ['slide across', 'with staggered', 'eases', 'on each line']
    eases = ['ease-out', 'ease-in-out', 'ease-in', 'linear']

    def slide_at(x):
        return [
            *stage_label(c, 4, 'text animation: slide'),
            *[{'type': 'text', 'id': f'slide-{i}', 'box': {'x': x, 'y': 40 + i * 26, 'w': 200, 'h': 26}, 'text': text, 'transition': {'durationMs': 700 + i * 250, 'easing': eases[i]}}
              for i, text in enumerate(slide)],
        ]
    steps += [{'elements': slide_at(20), 'holdMs': 300}, {'elements': slide_at(c['width'] - 220), 'holdMs': 2000}, {'elements': slide_at(20), 'holdMs': 1500}]

    captions = ['captions arrive at the bottom', 'older lines glide up', 'and the oldest one leaves', 'every step is one render', 'at any rate the link allows', 'until the transcript ends']
    rows = 3
    pitch = 26
    top = c['height'] - 12 - rows * pitch
    for step in range(len(captions)):
        visible = captions[max(0, step - rows + 1):step + 1]
        first = step - len(visible) + 1
        steps.append({
            'elements': [
                *stage_label(c, 4, 'text animation: captions'),
                {'type': 'rect', 'id': 'cap-box', 'box': {'x': 12, 'y': top - 6, 'w': c['width'] - 24, 'h': rows * pitch + 10}, 'style': {'border': 1, 'color': 5, 'radius': 6}},
                *[{'type': 'text', 'id': f'cap-{first + i}', 'box': {'x': 20, 'y': top + i * pitch, 'w': c['width'] - 40, 'h': pitch}, 'text': text, 'transition': {'durationMs': 330, 'easing': 'ease-out'}}
                  for i, text in enumerate(visible)],
            ],
            'holdMs': 550,
        })

    def fade(x, color, hold):
        return {
            'elements': [
                *stage_label(c, 4, 'text animation: fade'),
                {'type': 'text', 'id': 'fader', 'box': {'x': x, 'y': jround(c['height'] / 2), 'w': 300, 'h': 26}, 'text': 'fading in, drifting, fading out', 'style': {'color': color}, 'transition': {'durationMs': 1000, 'easing': 'linear'}},
            ],
            'holdMs': hold,
        }
    steps += [fade(40, 0, 200), fade(40, 15, 1300), fade(c['width'] - 340, 15, 1500), fade(c['width'] - 340, 0, 1300)]
    return steps


def stage_5_steps(c):
    track_w = c['width'] - 80

    def fr(ball, fill, gauge_end, readout, hold, transition):
        return {
            'elements': [
                *stage_label(c, 5, 'shape animation'),
                {'type': 'circle', 'id': 'ball', 'box': {'x': ball, 'y': jround(c['height'] / 2) - 24, 'w': 48, 'h': 48}, 'style': {'fill': True}, 'transition': transition},
                {'type': 'rect', 'id': 'track', 'box': {'x': 40, 'y': c['height'] - 40, 'w': track_w, 'h': 16}, 'style': {'border': 1, 'radius': 8, 'color': 6}},
                {'type': 'rect', 'id': 'bar', 'box': {'x': 40, 'y': c['height'] - 40, 'w': max(2, fill), 'h': 16}, 'style': {'fill': True, 'radius': 8, 'color': 12}, 'transition': transition},
                {'type': 'arc', 'id': 'gauge', 'box': {'x': c['width'] - 150, 'y': 40, 'w': 120, 'h': 120}, 'startDeg': -90, 'endDeg': gauge_end, 'style': {'width': 8}, 'transition': transition},
                {'type': 'text', 'id': 'readout', 'box': {'x': c['width'] - 150, 'y': 170, 'w': 120, 'h': 26}, 'text': readout},
            ],
            'holdMs': hold,
        }
    steps = [fr(40, 2, -90, '0 %', 0, STAGE_EASE['inOut'])]
    for i in range(1, 11):
        steps.append(fr(c['width'] - 90, track_w, 270, f'{i * 10} %', 180, STAGE_EASE['inOut']))
    steps[-1]['holdMs'] = 800
    for _ in range(2):
        steps += [fr(40, 2, -90, '0 %', 2000, STAGE_EASE['inOut']), fr(c['width'] - 90, track_w, 270, '100 %', 2000, STAGE_EASE['inOut'])]
    return steps


def stage_6_steps(c):
    def at(x, y, hold):
        return {
            'elements': [
                *stage_label(c, 6, 'composed glides'),
                {'type': 'circle', 'id': 'ball', 'box': {'x': x, 'y': y, 'w': 48, 'h': 48}, 'style': {'fill': True}, 'transition': {'durationMs': 1000, 'easing': 'ease-out'}},
                {'type': 'text', 'id': 'ball-label', 'box': {'x': x, 'y': y + 54, 'w': 240, 'h': 26}, 'text': 'second glide adds to the first', 'transition': {'durationMs': 1000, 'easing': 'ease-out'}},
            ],
            'holdMs': hold,
        }
    cx = jround(c['width'] / 2) - 24
    cy = jround(c['height'] / 2) - 40
    return [at(cx, cy, 0), at(jround(c['width'] * 0.15), 30, 400), at(jround(c['width'] * 0.1), c['height'] - 100, 2500)]


STAGE_CASES = [
    case('stage-1-shapes-card', 'stages', '1 · shapes card', 'Every shape type on one screen: line, disc, ring, rounded fill, triangle, quad, bezier, arc, pie.',
         lambda c: [{'elements': [*stage_label(c, 1, 'every shape at once'), *stage_shapes(c)], 'holdMs': 2500}],
         lambda c: unsupported(c, STAGE_SHAPE_IDS), blank_after=True),
    case('stage-2-text-lines', 'stages', '2 · text lines, gray levels, clipping', 'Three sentences, five gray levels, a line clipped by a narrow box, and non-ASCII text.',
         stage_2_steps, stage_2_expect, blank_after=True),
    case('stage-3-fast-text', 'stages', '3 · fast text updates', 'A counter, a typewriter and a tenths clock, one render per update with no hold; the rate shows how fast a small update lands.',
         stage_3_steps, lambda c: budgeted(c, stage_3_steps(c)[-1]['elements']), blank_after=True),
    case('stage-4-text-animation', 'stages', '4 · text animation', 'Four lines slide across with staggered eases, captions scroll up as new lines arrive, then a line fades in, drifts and fades out.',
         stage_4_steps, lambda c: OK, blank_after=True),
    case('stage-5-shape-animation', 'stages', '5 · shape animation with a text readout', 'A disc glides, a bar and an arc gauge tween, and a percentage readout ticks alongside; then everything runs back and forth.',
         stage_5_steps, lambda c: unsupported(c, [('ball', 'circle'), ('gauge', 'arc')]), blank_after=True),
    case('stage-6-composed-glides', 'stages', '6 · composed glides', 'A second move lands while the first is still in flight; on animating glasses the disc and its label blend the two into one path.',
         stage_6_steps, lambda c: unsupported(c, [('ball', 'circle')]), blank_after=True),
]

SHAPE_CASES += STAGE_CASES


def select_cases(selectors):
    if not selectors:
        return list(SHAPE_CASES)
    return [c for c in SHAPE_CASES if c['id'] in selectors or c['group'] in selectors]


# ============================================================================
# Runner
# ============================================================================

class Pipeline:
    """The host side of render() without the radio: anchors, validation, diffing."""

    def __init__(self):
        self.prev = []
        self.synthetic = 0

    def next_synthetic(self):
        self.synthetic += 1
        return f'~{self.synthetic}'

    def frame(self, elements):
        resolved = resolve_anchors(elements, CANVAS['width'], CANVAS['height'])
        processed = process_scene(resolved, CANVAS)
        diffed = diff_scene(self.prev, processed['elements'], self.next_synthetic)['elements']
        self.prev = diffed
        return processed, diffed


class Dump:
    """`--dump`: [1][len:u16][message] for every STREAM message of the left temple, [2][ms:u32] waits, [3][len:u8][label]."""

    def __init__(self):
        self.data = bytearray()

    def message(self, message):
        self.data += bytes([1]) + struct.pack('<H', len(message)) + message

    def wait(self, ms):
        if ms > 0:
            self.data += bytes([2]) + struct.pack('<I', int(ms))

    def label(self, label):
        encoded = label.encode('utf-8')[:255]
        self.data += bytes([3, len(encoded)]) + encoded


def parse_dump(data):
    """The dump back as ('message', bytes) / ('wait', ms) / ('label', str) items."""
    items, pos = [], 0
    while pos < len(data):
        tag = data[pos]
        if tag == 1:
            length = struct.unpack_from('<H', data, pos + 1)[0]
            items.append(('message', bytes(data[pos + 3:pos + 3 + length])))
            pos += 3 + length
        elif tag == 2:
            items.append(('wait', struct.unpack_from('<I', data, pos + 1)[0]))
            pos += 5
        elif tag == 3:
            length = data[pos + 1]
            items.append(('label', bytes(data[pos + 2:pos + 2 + length]).decode('utf-8')))
            pos += 2 + length
        else:
            raise ValueError(f'unknown dump tag {tag} at {pos}')
    return items


RESET_COMMANDS = [(OBJ_CONTROL, obj_control_payload()), (ASSET_CLEAR, b''), (OBJ_HIDE, obj_hide_payload(push=True))]
SETUP_MESSAGE = b''.join(record(sub, payload) for sub, payload in RESET_COMMANDS)


class Temple:
    def __init__(self, side, name, identity, link, sender):
        self.side = side
        self.name = name
        self.identity = identity
        self.link = link
        self.sender = sender


class Renderer:
    """The render() half of the host: one call = one frame on the glasses, resolved once acked."""

    def __init__(self, scenes, temples=None, trace=False, dump=None):
        self.scenes = scenes
        self.primary = next(iter(scenes))
        self.temples = temples or {}
        self.trace = trace
        self.dump = dump
        self.pipeline = Pipeline()
        self.last_ack_ms = 0
        self.last_bytes = 0
        self.last_messages = []
        self.last_settle_ms = 0
        self.last_hits = 0

    def resident(self):
        return self.scenes[self.primary].resident()

    async def render(self, elements):
        processed, diffed = self.pipeline.frame(elements)
        base = {'status': 'displayed', 'degraded': processed['degraded'], 'dropped': processed['dropped']}
        per_side = {side: messages(scene.encode(diffed)) for side, scene in self.scenes.items()}
        scene = self.scenes[self.primary]
        self.last_settle_ms = scene.anim_ms
        self.last_hits = scene.refs
        self.last_messages = per_side[self.primary]
        self.last_bytes = sum(len(m) for m in self.last_messages)
        if self.dump is not None:
            for message in self.last_messages:
                self.dump.message(message)
        started = time.perf_counter()
        await self._transmit(per_side)
        self.last_ack_ms = (time.perf_counter() - started) * 1000 if self.temples else 0
        if self.trace and diffed:
            parts = [f'{scene.sets} put' if scene.sets else '', f'{scene.refs} cached' if scene.refs else '',
                     f'{scene.tweens} tween' if scene.tweens else '', f'{scene.deletes} left' if scene.deletes else '',
                     f'{scene.directs} direct' if scene.directs else '']
            dropped = f"  dropped {','.join(processed['dropped'])}" if processed['dropped'] else ''
            print(f"      {self.last_bytes:>5} B  {round(self.last_ack_ms):>4} ms ack  settle {self.last_settle_ms} ms  "
                  f"{', '.join(p for p in parts if p) or 'no change'}{dropped}", flush=True)
        return {**base, 'presented': True}

    async def _transmit(self, per_side):
        if not self.temples:
            return
        try:
            for i in range(max(len(m) for m in per_side.values())):
                await asyncio.gather(*(self.temples[side].sender.send(msgs[i], timeout=ACK_TIMEOUT)
                                       for side, msgs in per_side.items() if i < len(msgs)))
        except Exception:
            # The mirrors no longer match the glasses: start the glasses and the mirrors over.
            for scene in self.scenes.values():
                scene.invalidate()
            self.pipeline.prev = []
            for temple in self.temples.values():
                try:
                    for sub, payload in RESET_COMMANDS:
                        await temple.link.check(sub, payload)
                except Exception:
                    pass
            raise


async def sleep_until(t):
    delay = t - time.perf_counter()
    if delay > 0:
        await asyncio.sleep(delay)


async def run_case(shapes_case, renderer, hold_scale, dump):
    head = {'id': shapes_case['id'], 'group': shapes_case['group'], 'title': shapes_case['title']}
    steps = shapes_case['steps'](CANVAS)
    expectation = shapes_case['expect'](CANVAS)
    ack_ms = settle_ms = total_bytes = 0
    started = time.perf_counter()

    def scaled(ms):
        return jround(ms * hold_scale) / 1000

    if dump is not None:
        dump.label(shapes_case['id'])
    try:
        # The glasses keep animating after a frame is acked; a later frame never
        # waits for that (it is the point of the compose cases), but the case's
        # last frame does before its hold, so nothing is blanked mid-transition.
        anim_ends_at = 0
        for i, step in enumerate(steps):
            sent_at = time.perf_counter()
            result = await renderer.render(step['elements'])
            acked_at = time.perf_counter()
            ack_ms = max(ack_ms, renderer.last_ack_ms)
            settle_ms = max(settle_ms, renderer.last_settle_ms)
            total_bytes += renderer.last_bytes
            anim_ends_at = max(anim_ends_at, acked_at + renderer.last_settle_ms / 1000)
            if i < len(steps) - 1:
                if dump is not None:
                    dump.wait(step['holdMs'])
                await sleep_until(sent_at + scaled(step['holdMs']))
                continue
            verdict = evaluate_result(result, expectation)
            elapsed = max(0.001, time.perf_counter() - started)
            if dump is not None:
                dump.wait(max(step['holdMs'], renderer.last_settle_ms))
            await sleep_until(max(sent_at + scaled(step['holdMs']), anim_ends_at if hold_scale > 0 else 0))
            if shapes_case['blankAfter']:
                await renderer.render([])
                if dump is not None:
                    dump.wait(700)
                await asyncio.sleep(scaled(700))
            return {
                **head,
                'state': 'pass' if verdict['pass'] else 'fail',
                'reason': verdict['reason'],
                'renderStatus': result['status'],
                'degraded': result['degraded'] is True,
                'dropped': result['dropped'],
                'presented': result['presented'],
                'ackMs': round(ack_ms),
                'settleMs': settle_ms,
                'steps': len(steps),
                'stepsPerSecond': jround(len(steps) / elapsed * 10) / 10,
                'bytes': total_bytes,
            }
        return {**head, 'state': 'error', 'reason': 'case has no steps'}
    except Exception as err:  # noqa: BLE001 - the case is reported, the run goes on
        return {**head, 'state': 'error', 'reason': f'{type(err).__name__}: {err}' if str(err) else type(err).__name__}


def parse_revision(identity):
    """`G1CFW/<n>` -> n, or None."""
    name, _, number = identity.partition('/')
    return int(number) if name == 'G1CFW' and number.isdigit() else None


async def open_temples(stack, opts, required=REQUIRED_REVISION):
    """Connect the temples named by opts.left / opts.right, require G1CFW/`required`, start them clean."""
    import g1flash
    from bleak import BleakClient, BleakScanner
    temples = {}
    for side, address in (('L', opts.left), ('R', opts.right)):
        if not address:
            continue
        names = {}

        def matches(device, advertisement, side=side, address=address):
            name = advertisement.local_name or device.name
            if device.address.casefold() == address.casefold() and g1flash.is_g1(name, side):
                names[device.address] = name
                return True
            return False

        device = await BleakScanner.find_device_by_filter(matches, timeout=opts.scan_seconds)
        if device is None:
            raise SystemExit(f'{side}: {address} was not found as a G1 {side} temple')
        client = await stack.enter_async_context(BleakClient(device, timeout=20))
        link = await stack.enter_async_context(g1flash.CfwLink(client))
        body = await link.check(g1flash.CFW_PING)
        identity = body.split(b'\0')[0].decode('ascii', 'replace')
        revision = parse_revision(identity)
        if revision is None or revision < required:
            raise SystemExit(f'{side}: {names[device.address]} answers {identity!r}; this suite needs G1CFW/{required} or later')
        for sub, payload in RESET_COMMANDS:
            await link.check(sub, payload)
        temples[side] = Temple(side, names[device.address], identity, link, g1flash.StreamSender(link))
        print(f'{side}: {names[device.address]} {identity}', flush=True)
    return temples


async def run_suite(opts, selected):
    dump = Dump() if opts.dump else None
    dry_run = opts.dry_run or opts.dump is not None
    hold_scale = 0 if dry_run else opts.hold_scale
    results = []
    temples = {}
    async with AsyncExitStack() as stack:
        if not dry_run:
            temples = await open_temples(stack, opts)
        sides = list(temples) or ['L']
        scenes = {side: G1Scene(1 if side == 'L' else -1) for side in sides}
        renderer = Renderer(scenes, temples, trace=opts.trace, dump=dump)
        if dump is not None:
            dump.label('setup')
            dump.message(SETUP_MESSAGE)
        print(f"{'dry run: ' if dry_run else ''}{len(selected)} case{'' if len(selected) == 1 else 's'}, "
              f"canvas {CANVAS['width']}×{CANVAS['height']}, budget {CANVAS['maxTextElements']}", flush=True)
        try:
            for i, shapes_case in enumerate(selected):
                r = await run_case(shapes_case, renderer, hold_scale, dump)
                results.append(r)
                timing = '' if r['state'] == 'error' else (
                    f"  {r['ackMs']:>4} ms ack, {r['settleMs']:>4} ms anim, {r['steps']} step{'' if r['steps'] == 1 else 's'} "
                    f"@ {r['stepsPerSecond']}/s, {r['bytes']} B, {renderer.resident()} cached")
                print(f"[{i + 1:>2}/{len(selected)}] {r['state'].upper():<5} {r['id']:<26} {r['reason']}{timing}", flush=True)
        finally:
            if temples:
                try:
                    await renderer.render([])
                except Exception:
                    pass
                if any(t.sender.deferred for t in temples.values()):
                    print('note: a temple reported the panel off; frames were written but not shown', flush=True)
            elif dump is not None:
                dump.label('teardown')
                await renderer.render([])
    if dump is not None:
        Path(opts.dump).write_bytes(bytes(dump.data))
        print(f'wrote {len(dump.data)} bytes to {opts.dump}', flush=True)
    passed = sum(1 for r in results if r['state'] == 'pass')
    failed = [r for r in results if r['state'] == 'fail']
    errored = [r for r in results if r['state'] == 'error']
    print(f'\n{passed} passed, {len(failed)} failed, {len(errored)} errored', flush=True)
    for r in failed + errored:
        print(f"  {r['state'].upper()} {r['id']}: {r['reason']}", flush=True)
    if opts.out:
        out = {
            'ranAt': datetime.now(timezone.utc).isoformat(),
            'dryRun': dry_run,
            'temples': {side: {'name': t.name, 'cfw': t.identity} for side, t in temples.items()} or None,
            'canvas': CANVAS,
            'holdScale': hold_scale,
            'results': results,
        }
        Path(opts.out).write_text(json.dumps(out, indent=2) + '\n')
        print(f'wrote {opts.out}', flush=True)
    return results


# ============================================================================
# Self-test: encoder vectors, byte for byte
# ============================================================================

def self_test_checks():
    """(name, got, want) triples over G1Scene output for simple frames."""
    checks = []

    def eq(name, got, want):
        checks.append((name, got, want))

    def le(*values):
        return struct.pack(f'<{len(values)}h', *values)

    def fresh():
        return Pipeline(), G1Scene(1)

    def shown(pipeline, scene, elements):
        return scene.encode(pipeline.frame(elements)[1])

    # shapes become kind-4 PUTs with ids from 1, shown in order
    pipeline, scene = fresh()
    records = shown(pipeline, scene, [
        {'type': 'line', 'id': 'l', 'points': [{'x': 10, 'y': 20}, {'x': 30, 'y': 40}], 'style': {'width': 3, 'color': 12}},
        {'type': 'circle', 'id': 'c', 'box': {'x': 100, 'y': 100, 'w': 41, 'h': 41}, 'style': {'fill': True}},
        {'type': 'arc', 'id': 'a', 'box': {'x': 0, 'y': 0, 'w': 101, 'h': 101}, 'startDeg': -90, 'endDeg': 180, 'style': {'width': 4}},
        {'type': 'rect', 'id': 'r', 'box': {'x': 1, 'y': 2, 'w': 30, 'h': 20}, 'style': {'border': 2, 'radius': 6, 'fill': True}},
    ])
    eq('five puts and a show', len(records), 6)
    eq('line put', records[0], bytes.fromhex('201a00 01000100 0401 01000c03') + le(10, 20, 30, 40, 0, 0, 0, 0))
    eq('circle put', records[1], bytes.fromhex('201a00 02000100 0401 05000f00') + le(120, 120, 20, 0, 0, 0, 0, 0))
    eq('arc put', records[2], bytes.fromhex('201a00 03000100 0401 0c000f04') + le(50, 50, 50, -90, 180, 0, 0, 0))
    eq('rect fill put', records[3], bytes.fromhex('201a00 04000100 0401 03000f00') + le(1, 2, 30, 20, 6, 0, 0, 0))
    eq('rect stroke put', records[4], bytes.fromhex('201a00 05000100 0401 02000f02') + le(1, 2, 30, 20, 6, 0, 0, 0))
    eq('show clears and pushes five ids', records[5], bytes.fromhex('210c00 0305 0100 0200 0300 0400 0500'))
    eq('slot ids', scene.slot_ids(), [[1], [2], [3], [4, 5]])
    eq('resident', scene.resident(), 5)
    eq('one message', [len(m) for m in messages(records)], [5 * 29 + 15])

    # a moved element with a transition is a SHOW of the same list plus one tween entry with every target value
    pipeline, scene = fresh()
    shown(pipeline, scene, [{'type': 'circle', 'id': 'ball', 'box': {'x': 0, 'y': 0, 'w': 21, 'h': 21}, 'style': {'fill': True}}])
    records = shown(pipeline, scene, [{'type': 'circle', 'id': 'ball', 'box': {'x': 200, 'y': 0, 'w': 21, 'h': 21}, 'style': {'fill': True},
                                       'transition': {'durationMs': 330, 'easing': 'linear'}}])
    eq('tween is a show and an anim', len(records), 2)
    eq('tween show', records[0], bytes.fromhex('210400 0301 0100'))
    eq('tween anim', records[1], bytes.fromhex('251000 2101 0100 0701 0a00') + le(210, 10, 10, 15))
    eq('tween counters', (scene.tweens, scene.refs, scene.sets, scene.anim_ms), (1, 1, 0, 330))

    # a geometry change without a transition is a new version (one PUT, no anim); a type change too
    pipeline, scene = fresh()
    shown(pipeline, scene, [{'type': 'circle', 'id': 's', 'box': {'x': 0, 'y': 0, 'w': 11, 'h': 11}}])
    records = shown(pipeline, scene, [{'type': 'circle', 'id': 's', 'box': {'x': 5, 'y': 0, 'w': 11, 'h': 11}}])
    eq('moved without transition re-PUTs', records[0], bytes.fromhex('201a00 01000200 0401 04000f01') + le(10, 5, 5, 0, 0, 0, 0, 0))
    eq('moved without transition has no anim', len(records), 2)
    records = shown(pipeline, scene, [{'type': 'circle', 'id': 's', 'box': {'x': 5, 'y': 0, 'w': 11, 'h': 11}, 'style': {'fill': True}, 'transition': {'durationMs': 500}}])
    eq('retyped', records[0], bytes.fromhex('201a00 01000300 0401 05000f00') + le(10, 5, 5, 0, 0, 0, 0, 0))
    eq('retyped has no anim', len(records), 2)

    # removal drops the object and frees its id; a blank frame hides; a re-show from the mirror is one SHOW
    pipeline, scene = fresh()
    a = {'type': 'circle', 'id': 'a', 'box': {'x': 0, 'y': 0, 'w': 11, 'h': 11}}
    b = {'type': 'circle', 'id': 'b', 'box': {'x': 0, 'y': 0, 'w': 11, 'h': 11}}
    shown(pipeline, scene, [a, b])
    records = shown(pipeline, scene, [b])
    eq('removal drops a and shows b', records, [bytes.fromhex('240300 01 0100'), bytes.fromhex('210400 0301 0200')])
    eq('a gone', scene.resident(), 1)
    records = shown(pipeline, scene, [a, b])
    eq('a comes back as id 1', records[0][:9], bytes.fromhex('201a00 01000100 0401'))
    eq('a comes back show', records[1], bytes.fromhex('210600 0302 0100 0200'))
    eq('a comes back counters', (scene.sets, scene.refs), (1, 1))
    records = shown(pipeline, scene, [])
    eq('blank hides', records, [bytes.fromhex('220100 02')])
    records = shown(pipeline, scene, [a, b])
    eq('reopen from the mirror is one show', records, [bytes.fromhex('210600 0302 0100 0200')])
    eq('reopen counters', (scene.refs, scene.sets), (2, 0))
    eq('no change sends nothing', shown(pipeline, scene, [a, b]), [])

    # text is a string asset plus a kind-1 object; a move with a transition tweens x,y; new wording replaces the asset
    pipeline, scene = fresh()

    def t(y, text, color, transition=None):
        el = {'type': 'text', 'id': 't', 'box': {'x': 10, 'y': y, 'w': 200, 'h': 60}, 'text': text, 'style': {'color': color}}
        return {**el, 'transition': transition} if transition else el
    records = shown(pipeline, scene, [t(10, 'hi\nthere', 9)])
    eq('text asset', records[0], bytes.fromhex('081000 0100 02 00 0000 0000') + b'hi\nthere')
    eq('text put', records[1], bytes.fromhex('201300 01000100 0101') + struct.pack('<hhHHHBBB', 10, 10, 200, 60, 1, 15, 0, 2))
    eq('text show', records[2], bytes.fromhex('210400 0301 0100'))
    records = shown(pipeline, scene, [t(40, 'hi\nthere', 9, {'durationMs': 330})])
    eq('text move tweens x,y on the ease curve', records, [bytes.fromhex('210400 0301 0100'), bytes.fromhex('251000 2101 0100 0300 0a80') + le(10, 40) + bytes.fromhex('401a40ff')])
    records = shown(pipeline, scene, [t(40, 'ho\nthere', 9, {'durationMs': 330})])
    eq('text change replaces the asset', records[0], bytes.fromhex('081000 0100 02 00 0000 0000') + b'ho\nthere')
    eq('text change re-PUTs', records[1], bytes.fromhex('201300 01000200 0101') + struct.pack('<hhHHHBBB', 10, 40, 200, 60, 1, 15, 0, 2))
    eq('text change counters', (scene.sets, scene.tweens), (1, 0))
    eq('text colour is not drawn', shown(pipeline, scene, [t(40, 'ho\nthere', 3, {'durationMs': 330})]), [])

    # colour and stroke tween together with the geometry
    pipeline, scene = fresh()

    def ring(color, stroke):
        return {'type': 'circle', 'id': 'ring', 'box': center(CANVAS, 140, 140), 'style': {'border': stroke, 'color': color}, 'transition': {'durationMs': 700}}
    shown(pipeline, scene, [ring(2, 1)])
    records = shown(pipeline, scene, [ring(15, 12)])
    eq('colour and stroke tween', records[1], bytes.fromhex('251600 2101 0100 0703 1580') + le(320, 100, 69, 15, 12) + bytes.fromhex('401a40ff'))

    # past the retained budget the rest is drawn after the SHOW and flushed
    pipeline, scene = fresh()
    discs = [{'type': 'circle', 'id': f'd{i}', 'box': {'x': 8 + (i % 26) * 24, 'y': 8 + (i // 26) * 24, 'w': 20, 'h': 20}, 'style': {'fill': True}} for i in range(95)]
    records = shown(pipeline, scene, discs)
    eq('budget: 90 puts, a show, 5 direct shapes, a flush', len(records), 97)
    eq('budget show does not push', records[90][:5], bytes.fromhex('21b600 015a'))
    eq('budget direct shape', records[91], bytes.fromhex('0d1600 0001 05000f00') + le(306, 90, 9, 0, 0, 0, 0, 0))
    eq('budget flush', records[-1], bytes.fromhex('030800 0000 0000 8002 c800'))
    eq('budget resident', scene.resident(), 90)
    eq('budget slot ids', (scene.slot_ids()[89], scene.slot_ids()[90]), ([90], []))

    # G1CFW/39: an svg element is a path asset and a kind-5 object; a rotated shape snaps its rotation after the PUT
    pipeline, scene = fresh()
    records = shown(pipeline, scene, [{'type': 'svg', 'id': 'p', 'box': {'x': 10, 'y': 20, 'w': 100, 'h': 50}, 'path': svg_path('M0 0H60V30H0Z'), 'fillRule': 1, 'style': {'color': 9}}])
    commands = b'\x00' + le(0, 0) + b'\x01' + le(960, 0) + b'\x01' + le(960, 480) + b'\x01' + le(0, 480) + b'\x04'
    eq('path asset', records[0], bytes.fromhex('081d00 0100 03 00 0100 0000') + commands)
    eq('path object', records[1], bytes.fromhex('201300 01000100 0501') + struct.pack('<hhHHHBBB', 10, 20, 256, 0, 1, 9, 0, 0))
    pipeline, scene = fresh()
    records = shown(pipeline, scene, [{'type': 'rect', 'id': 'bar', 'box': {'x': 300, 'y': 80, 'w': 100, 'h': 20}, 'style': {'fill': True}, 'rotationQ8': 90 * 256}])
    eq('rotated put snaps the angle about the box centre', records[2], bytes.fromhex('250e00 2101 0100 001c 0000') + le(1440, 350, 90))

    # framing, stream packets and the small codecs
    eq('crc16 check value', crc16(b'123456789'), 0x29B1)
    message = bytes(range(256)) * 2
    packets, seq = stream_packets(message, 7)
    eq('stream packet sizes', [len(p) for p in packets], [255, 255, 18])
    eq('stream packet flags', [p[3] for p in packets], [1, 0, 2])
    eq('stream packet seq', ([p[2] for p in packets], seq), ([7, 8, 9], 10))
    eq('stream header', packets[0][4:8], struct.pack('<HH', 512, crc16(message)))
    eq('stream data', b''.join(p[4:] for p in packets)[4:], message)
    eq('messages split at 4096', [len(m) for m in messages([b'a' * 4000, b'b' * 97, b'c' * 96])], [4000, 97 + 96])
    eq('messages fill to 4096', [len(m) for m in messages([b'a' * 4000, b'b' * 96])], [4096])
    eq('frames', [transition_frames(ms) for ms in (0, 10, 330, 400, 700, 10000)], [0, 1, 10, 12, 21, 255])
    eq('easing curves', [easing_curve(e) for e in ('linear', 'ease-out', 'ease-in', 'ease-in-out', 'ease', None)],
       [None, (0, 0, 148, 255), (107, 0, 255, 255), (107, 0, 148, 255), (64, 26, 64, 255), (64, 26, 64, 255)])
    # the host test's `cur_lin` entry: a linear curve rides as 0x80 + four bytes; code 0 still encodes without them
    eq('curve entry', anim_entry(6, 0x01, 4, 0, [500], (0, 0, 255, 255)), bytes.fromhex('0600 0100 0480 f401 0000ffff'))
    eq('code 0 entry', anim_entry(6, 0x01, 4, 0, [500]), bytes.fromhex('0600 0100 0400 f401'))
    eq('reset records', RESET_RECORDS, [bytes.fromhex('240100 00'), bytes.fromhex('0b0000')])
    eq('shape types', (LINE, RECT, RECT_FILL, CIRCLE, CIRCLE_FILL, ARC, PIE), (1, 2, 3, 4, 5, 12, 13))
    return checks


def self_test():
    checks = self_test_checks()
    failures = [(name, got, want) for name, got, want in checks if got != want]
    for name, got, want in failures:
        print(f'  FAIL {name}: got {got!r}, want {want!r}')
    print(f'self-test: {len(checks) - len(failures)}/{len(checks)} checks ok')
    return not failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('selectors', nargs='*', help='case ids or groups (default: every case)')
    parser.add_argument('--list', action='store_true', help='print ids and groups, no connection')
    parser.add_argument('--dry-run', action='store_true', help='host pipeline only, no glasses (also G1_DRY_RUN=1)')
    parser.add_argument('--dump', metavar='FILE', help='write every message and wait for the host replay (implies --dry-run)')
    parser.add_argument('--self-test', action='store_true', help='encoder vectors, byte for byte')
    parser.add_argument('--out', metavar='FILE', help='write the results as JSON')
    parser.add_argument('--hold-scale', type=float, default=1.0, help='multiply every hold (default 1)')
    parser.add_argument('--trace', action='store_true', help='per-frame op counts and timings (also G1_TRACE=1)')
    parser.add_argument('--left', metavar='UUID', help='left temple address from g1flash.py scan')
    parser.add_argument('--right', metavar='UUID', help='right temple address from g1flash.py scan')
    parser.add_argument('--scan-seconds', type=float, default=10)
    opts = parser.parse_args(argv)
    if opts.list:
        for c in SHAPE_CASES:
            print(f"{c['id']:<28} {c['group']:<10} {c['title']}")
        return 0
    if opts.self_test:
        return 0 if self_test() else 1
    if os.environ.get('G1_DRY_RUN') == '1':
        opts.dry_run = True
    if os.environ.get('G1_TRACE') == '1':
        opts.trace = True
    if opts.hold_scale < 0:
        parser.error('--hold-scale must be a non-negative number')
    if not (opts.dry_run or opts.dump or opts.left or opts.right):
        parser.error('connect with --left/--right UUID, or use --dry-run')
    selected = select_cases(opts.selectors)
    if not selected:
        print(f"no cases match {', '.join(opts.selectors)}; try --list", file=sys.stderr)
        return 2
    results = asyncio.run(run_suite(opts, selected))
    return 1 if any(r['state'] != 'pass' for r in results) else 0


if __name__ == '__main__':
    sys.exit(main())
