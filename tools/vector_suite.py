#!/usr/bin/env python3
"""The g2flash vector suite (filled SVG paths and rotation) on the G1's retained objects.

The fixtures are g2flash/demos/vector-cases.ts step for step, kept in their
640x480 coordinates; every absolute coordinate, length, pivot and probe is
scaled by 200/480 when the bytes are built so the whole suite stays on the
640x200 canvas. Each step is one STREAM message of G1 primitives: slot n is
object id n + 1 (and path asset id n + 1); a scene PUTs new or changed
definitions (ASSET_PUT kind 3 + OBJ_PUT kind 5 for paths, OBJ_PUT kind 4 for
shapes), SHOWs the slot list and carries the ops as one OBJ_ANIM entry per
object (the firmware keeps one entry per object, so a glide and a rotation
of the same slot merge into one mask; the longer frame count wins). The
pivot is a tween value on the glasses, so a rotation that starts from 0 snaps
its pivot in a first ANIM record and glides only the angle in a second.

What the G1 cannot express: `freeze` (a running tween cannot be stopped where
it is; nothing is sent and the tween completes) and a `finish` of a tween
this port did not send. `finish` is sent as a 0-frame snap to the targets of
the last entries this port sent for the slot, which is what the G2 does with
its running tweens.

    tools/vector_suite.py --list
    tools/vector_suite.py --dump /tmp/vector.dump      # for tests/host/g1cfw_host_test.c --replay
    tools/vector_suite.py --dry-run                    # encode only, print notes
    tools/vector_suite.py --left UUID --right UUID     # visual checks on the glasses

Needs G1CFW/39 or later.
"""
import argparse
import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cfw.scene import (ANIM_MAX_ENTRIES, ASSET_PUT, KIND_PATH, KIND_SHAPE, MSG_MAX, OBJ_ANIM, OBJ_HIDE, OBJ_PUT,  # noqa: E402
                       OBJ_SHOW, PATH_TWEEN_MASK, RECT_FILL, RESET_RECORDS, SHAPE_COORD_MASK, SHAPE_X_PARAMS,
                       anim_entry, jround, obj_anim_payload, obj_hide_payload, obj_put_payload, obj_put_shape_payload,
                       obj_show_payload, pack_path_commands, path_asset_payload, path_commands, record, rotation_masks, shape_record,
                       svg_path)
from tools.shapes_suite import open_temples  # noqa: E402

REQUIRED_REVISION = 39
SCALE = 200 / 480
# CSS cubic-bezier control points scaled to 0..255 (object-cache.ts EASE); from G1CFW/40 the
# glasses take them verbatim (easing byte 0x80 + the four bytes), so the fixtures' curves pass through.
EASE_LINEAR, EASE_IN_OUT, EASE_OUT, EASE_IN, EASE_EASE = (0, 0, 255, 255), (107, 0, 148, 255), (0, 0, 148, 255), (107, 0, 255, 255), (64, 26, 64, 255)


# ============================================================================
# Fixtures: vector-protocol.ts helpers and vector-cases.ts, in G2 coordinates
# ============================================================================

def shape(slot, type_, params, color=15, width=1):
    return {'slot': slot, 'kind': KIND_SHAPE, 'type': type_, 'params': (list(params) + [0] * 8)[:8], 'color': color, 'width': width}


def path(slot, commands, rule='nonzero', x=0, y=0, scale=1, color=15):
    return {'slot': slot, 'kind': KIND_PATH, 'commands': bytes(commands), 'rule': 1 if rule == 'evenodd' else 0,
            'params': [x, y, jround(scale * 256), 0, 0, 0, 0, 0], 'color': color, 'width': 0}


def rotate(slot, degrees, px, py, duration_ms=0, curve=EASE_LINEAR):
    return {'op': 'rotate', 'slot': slot, 'angle': jround(degrees * 16), 'px': px, 'py': py,
            'frames': min(255, jround(duration_ms / 33)), 'curve': tuple(curve)}


def glide(slot, dx, dy, frames, curve=EASE_LINEAR):
    return {'op': 'glide', 'slot': slot, 'dx': dx, 'dy': dy, 'frames': frames, 'curve': tuple(curve)}


def tween(slot, mask, frames, curve, values):
    return {'op': 'tween', 'slot': slot, 'mask': mask, 'frames': frames, 'curve': tuple(curve), 'values': list(values)}


def freeze(slot):
    return {'op': 'freeze', 'slot': slot}


def finish(slot):
    return {'op': 'finish', 'slot': slot}


RING = 'M80 80H240V240H80Z M120 120H200V200H120Z'
SHAPE_PARAMS = {
    1: [0, 0, 75, 60], 2: [0, 0, 80, 60, 15], 3: [0, 0, 80, 60, 15], 4: [40, 30, 28], 5: [40, 30, 28],
    6: [0, 60, 40, 0, 80, 60], 7: [0, 60, 40, 0, 80, 60], 8: [0, 0, 70, 10, 80, 60, 10, 70], 9: [0, 0, 70, 10, 80, 60, 10, 70],
    10: [0, 60, 40, -30, 80, 60], 11: [0, 60, 10, -20, 70, -20, 80, 60], 12: [40, 30, 30, 0, 260], 13: [40, 30, 30, 0, 260],
}


def geometries():
    """Every geometric type on a 5-column grid, with its rotation pivot."""
    out = []
    for type_, p in SHAPE_PARAMS.items():
        col, row = (type_ - 1) % 5, (type_ - 1) // 5
        ox, oy = 40 + col * 120, 80 + row * 120
        count = 2 if type_ == 1 else 3 if type_ in (6, 7, 10) else 4 if type_ in (8, 9, 11) else 1
        v = list(p)
        for i in range(count):
            v[2 * i] += ox
            v[2 * i + 1] += oy
        out.append({'type': type_, 'v': v, 'px': ox + 40, 'py': oy + 30})
    return out


def vector_cases():
    """Every step is one message. A step that defines slots SHOWs them (previous
    slots leave the list but stay resident); a step with only ops keeps the list.
    The first step resets the glasses."""
    steps = [{'name': 'reset', 'reset': True, 'waitMs': 0}]

    def add(name, defs, ops=(), wait_ms=700, probes=None, keep_refs=None):
        steps.append({'name': name, 'defs': list(defs), 'ops': list(ops), 'keep': not defs and not keep_refs, 'refs': list(keep_refs or []),
                      'waitMs': wait_ms, 'probes': [tuple(p) for p in probes or []], 'snapshot': True})

    ring = svg_path(RING)
    add('fill-evenodd-hole', [path(0, ring, 'evenodd')], [], 700, [[90, 90, 15], [160, 160, 0]])
    add('fill-nonzero-same-winding', [path(0, ring)], [], 700, [[160, 160, 15]])
    add('fill-nonzero-opposite-winding', [path(0, svg_path('M80 80H240V240H80Z M120 120V200H200V120Z'))], [], 700, [[160, 160, 0]])
    add('fill-quadratic-cubic', [path(0, svg_path('M100 230C100 30 300 30 300 230Q200 350 100 230Z'))], [], 700, [[200, 170, 15], [200, 30, 0]])
    box = shape(0, RECT_FILL, [100, 100, 80, 40])
    add('rotation-initial', [box], [], 500, [[105, 120, 15], [140, 85, 0]])
    add('rotation-quarter-turn', [], [rotate(0, 90, 140, 120, 700)], 800, [[105, 120, 0], [140, 85, 15]])
    add('rotation-full-turn', [], [rotate(0, 450, 140, 120, 1800)], 1900, [[105, 120, 0], [140, 85, 15]])
    add('rotation-negative-fraction', [], [rotate(0, -37.5, 140, 120, 900)], 1000)
    add('rotation-retarget-start', [], [rotate(0, 720, 140, 120, 3000)], 650)
    add('rotation-retarget-and-move', [], [glide(0, 220, 100, 40), rotate(0, 0, 140, 120, 1200)], 1500, [[330, 230, 15]])
    add('rotation-freeze-start', [], [rotate(0, 360, 360, 220, 3000)], 400)
    add('rotation-freeze', [], [freeze(0)], 500)
    add('rotation-finish-start', [], [rotate(0, 0, 360, 220, 3000)], 300)
    add('rotation-finish', [], [finish(0)], 600, [[330, 230, 15]])
    rect = path(0, svg_path('M0 0H60V30H0Z'), 'nonzero', 100, 100)
    add('path-tween-initial', [rect], [], 500)
    add('path-move-scale-color-rotate', [], [tween(0, 0x107, 30, EASE_LINEAR, [300, 180, 512, 9]), rotate(0, 90, 300, 180, 990)], 1100, [[270, 220, 9], [330, 190, 0]])
    add('path-atomic-replacement', [path(0, svg_path(RING), 'evenodd')], [], 700, [[160, 160, 0], [90, 90, 15]])
    steps.append({'name': 'path-hide', 'hide': True, 'waitMs': 400, 'probes': [(90, 90, 0)], 'snapshot': True})
    shapes = geometries()
    add('all-geometric-shapes-initial', [shape(19 + g['type'], g['type'], g['v'], 15, 3) for g in shapes], [], 700)
    add('all-geometric-shapes-rotate-halfway', [], [rotate(19 + g['type'], 360, g['px'], g['py'], 2500) for g in shapes], 900)
    steps.append({'name': 'all-geometric-shapes-rotate-finish', 'defs': [], 'ops': [], 'keep': True, 'refs': [], 'waitMs': 1800, 'probes': [], 'snapshot': True})
    # the path from before is still resident: reopening it is one SHOW without a definition
    add('path-reopen-from-cache', [], [], 700, [[90, 90, 15], [160, 160, 0]], [path(0, svg_path(RING), 'evenodd')])
    steps.append({'name': 'release', 'hide': True, 'waitMs': 0, 'probes': [], 'snapshot': False})
    return steps


# ============================================================================
# Encoder: the fixtures' slots as G1 objects, scaled to the canvas
# ============================================================================

class SlotState:
    def __init__(self):
        self.definition = None     # the def resident on the glasses
        self.version = 0
        self.asset = None          # (commands, rule) resident in the slot's path asset
        self.params = None         # logical p0..p7 the glasses hold or glide to
        self.color = 15
        self.width = 0
        self.rotation = (0, 0, 0)  # angle Q4, pivot x, pivot y (logical)
        self.sent_mask = 0         # union of the tween masks sent since the PUT: what `finish` snaps


class VectorScene:
    """Mirror of the fixtures' slots: slot n is object id n + 1 and path asset id n + 1.
    Definitions and op targets stay in the fixtures' coordinates; `scale` is applied
    to every canvas length when the bytes are built."""

    def __init__(self, scale=SCALE):
        self.scale = scale
        self.slots = {}

    def sc(self, value):
        return jround(value * self.scale)

    def reset(self):
        self.slots = {}
        return list(RESET_RECORDS) + [record(OBJ_HIDE, obj_hide_payload(push=True))]

    def hide(self):
        return [record(OBJ_HIDE, obj_hide_payload(push=True))]

    def scene(self, defs, ops=(), keep=False, refs=()):
        """Records for one step and the notes on what could not be expressed exactly."""
        records, notes = [], []
        for d in sorted(defs, key=lambda d: d['slot']):
            records += self._define(d)
        for r in refs:
            state = self.slots.get(r['slot'])
            if state is None or state.definition != r:
                notes.append(f"slot {r['slot']}: referenced but not resident as given; defined")
                records += self._define(r)
        if not keep:
            shown = sorted({d['slot'] for d in [*refs, *defs]})
            records.append(record(OBJ_SHOW, obj_show_payload([s + 1 for s in shown], clear=True, push=True)))
        # Snaps (frames 0) go in a record before the glides: a pivot must land before its angle starts moving.
        snaps, glides = {}, {}
        for op in ops:
            self._apply(op, snaps, glides, notes)
        for table in (snaps, glides):
            entries = [anim_entry(slot + 1, p['mask'], p['frames'], 0, self._values(slot, p['mask']), p['curve']) for slot, p in table.items()]
            for i in range(0, len(entries), ANIM_MAX_ENTRIES):
                records.append(record(OBJ_ANIM, obj_anim_payload(entries[i:i + ANIM_MAX_ENTRIES])))
        return records, notes

    def _define(self, d):
        state = self.slots.setdefault(d['slot'], SlotState())
        if state.definition == d:
            return []
        oid = d['slot'] + 1
        out = []
        state.version += 1
        if d['kind'] == KIND_PATH:
            if state.asset != (d['commands'], d['rule']):
                out.append(record(ASSET_PUT, path_asset_payload(oid, self._scaled_commands(d['commands']), d['rule'])))
                state.asset = (d['commands'], d['rule'])
            x, y, scale = d['params'][:3]
            out.append(record(OBJ_PUT, obj_put_payload(oid, state.version, KIND_PATH, self.sc(x), self.sc(y), scale, 0, oid, d['color'])))
        else:
            out.append(record(OBJ_PUT, obj_put_shape_payload(oid, state.version, shape_record(d['type'], d['color'], d['width'], self._scaled_params(d, d['params'])))))
        state.definition = d
        state.params = list(d['params'])
        state.color, state.width = d['color'], d['width']
        state.rotation = (0, 0, 0)
        state.sent_mask = 0
        return out

    def _scaled_commands(self, commands):
        return pack_path_commands([(op, [self.sc(v) for v in coords]) for op, coords in path_commands(commands)])

    def _scaled_params(self, d, params):
        coords = PATH_TWEEN_MASK & 0x03 if d['kind'] == KIND_PATH else SHAPE_COORD_MASK[d['type']]
        return [self.sc(v) if coords & (1 << k) else v for k, v in enumerate(params)]

    def _values(self, slot, mask):
        state = self.slots[slot]
        angle, px, py = state.rotation
        values = self._scaled_params(state.definition, state.params) + [state.color, state.width, angle, self.sc(px), self.sc(py)]
        return [values[k] for k in range(13) if mask & (1 << k)]

    def _apply(self, op, snaps, glides, notes):
        slot = op['slot']
        state = self.slots.get(slot)
        if state is None:
            raise ValueError(f"{op['op']} on undefined slot {slot}")

        def merge(mask, frames, curve):
            table = glides if frames else snaps
            p = table.setdefault(slot, {'mask': 0, 'frames': frames, 'curve': curve})
            if p['mask'] and p['frames'] != frames:
                notes.append(f"slot {slot}: {op['op']} over {frames} frames merged with an entry over {p['frames']}; the longer wins")
            p['mask'] |= mask
            p['frames'] = max(p['frames'], frames)
            p['curve'] = curve
            state.sent_mask |= mask

        kind = op['op']
        if kind == 'rotate':
            want = (op['angle'], op['px'], op['py'])
            if want[0] == 0:
                want = (0,) + state.rotation[1:]   # the pivot is moot at angle 0: keep the glasses' value
            snap_mask, glide_mask = rotation_masks(state.rotation, want, op['frames'])
            state.rotation = want
            if snap_mask:
                # a pivot snapped ahead of its glide is this port's own entry; a 0-frame rotate keeps the fixture's curve
                merge(snap_mask, 0, None if glide_mask else op['curve'])
            if glide_mask:
                merge(glide_mask, op['frames'], op['curve'])
        elif kind == 'glide':
            xs = (0,) if state.definition['kind'] == KIND_PATH else SHAPE_X_PARAMS[state.definition['type']]
            mask = 0
            for i in xs:
                state.params[i] += op['dx']
                state.params[i + 1] += op['dy']
                mask |= 0x03 << i
            merge(mask, op['frames'], op['curve'])
        elif kind == 'tween':
            values = list(op['values'])
            for k in range(10):
                if op['mask'] & (1 << k):
                    v = values.pop(0)
                    if k < 8:
                        state.params[k] = v
                    elif k == 8:
                        state.color = v
                    else:
                        state.width = v
            merge(op['mask'], op['frames'], op['curve'])
        elif kind == 'freeze':
            notes.append(f'slot {slot}: freeze cannot stop a running tween where it is; nothing sent, the tween completes')
        elif kind == 'finish':
            if state.sent_mask:
                merge(state.sent_mask, 0, None)   # snap to the targets this port last sent
            else:
                notes.append(f'slot {slot}: finish with no tween sent; nothing to snap')
        else:
            raise ValueError(f'unknown op {kind}')


def encode_steps(steps, scale=SCALE):
    """Each step with its message (empty when nothing needs sending), scaled probes and notes."""
    scene = VectorScene(scale)
    out = []
    for step in steps:
        notes = []
        if step.get('reset'):
            records = scene.reset()
        elif step.get('hide'):
            records = scene.hide()
        else:
            records, notes = scene.scene(step['defs'], step['ops'], step['keep'], step['refs'])
        message = b''.join(records)
        if len(message) > MSG_MAX:
            raise ValueError(f"{step['name']}: {len(message)} bytes exceed one message")
        probes = [(scene.sc(x), scene.sc(y), level) for x, y, level in step.get('probes', [])]
        out.append({'name': step['name'], 'message': message, 'waitMs': step['waitMs'], 'probes': probes,
                    'snapshot': step.get('snapshot', False), 'notes': notes})
    return out


def dump_bytes(encoded):
    """[3][len][label], [1][len:u16][message], [2][ms:u32], [4][x:u16][y:u16][level] per probe, [5] for a snapshot."""
    out = bytearray()
    for step in encoded:
        label = step['name'].encode('utf-8')[:255]
        out += bytes([3, len(label)]) + label
        if step['message']:
            out += bytes([1]) + struct.pack('<H', len(step['message'])) + step['message']
        out += bytes([2]) + struct.pack('<I', jround(step['waitMs']))
        for x, y, level in step['probes']:
            out += bytes([4]) + struct.pack('<HHB', x, y, level)
        if step['snapshot']:
            out += bytes([5])
    return bytes(out)


async def run_device(opts, encoded):
    async with AsyncExitStack() as stack:
        temples = await open_temples(stack, opts, required=REQUIRED_REVISION)
        sent, worst = 0, 0
        for step in encoded:
            if step['name'] == 'reset':
                continue   # the connection started clean
            ack = ''
            if step['message']:
                started = time.perf_counter()
                await asyncio.gather(*(t.sender.send(step['message']) for t in temples.values()))
                ms = (time.perf_counter() - started) * 1000
                worst = max(worst, ms)
                sent += 1
                ack = f', {ms:.0f} ms ack'
            probes = f" ({len(step['probes'])} host pixel probes)" if step['probes'] else ''
            print(f"{step['name']}: {len(step['message'])} B{ack}; inspect expected display{probes}", flush=True)
            if step['name'] != 'release' and step['waitMs'] > 0:
                await asyncio.sleep(step['waitMs'] / 1000)
        print(f'{sent} messages acked; worst ack {worst:.0f} ms', flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--list', action='store_true', help='print the step names')
    parser.add_argument('--dump', metavar='FILE', help='write the replay stream for the host test')
    parser.add_argument('--dry-run', action='store_true', help='encode only')
    parser.add_argument('--left', metavar='UUID', help='left temple address from g1flash.py scan')
    parser.add_argument('--right', metavar='UUID', help='right temple address from g1flash.py scan')
    parser.add_argument('--scan-seconds', type=float, default=10)
    opts = parser.parse_args(argv)
    steps = vector_cases()
    if opts.list:
        print('\n'.join(step['name'] for step in steps))
        return 0
    encoded = encode_steps(steps)
    for step in encoded:
        for note in step['notes']:
            print(f"note: {step['name']}: {note}")
    if opts.dump:
        data = dump_bytes(encoded)
        Path(opts.dump).write_bytes(data)
        print(f'wrote {opts.dump}: {len(encoded)} steps, {len(data)} bytes')
    if not (opts.left or opts.right) or opts.dry_run:
        print(f"offline: {len(encoded)} steps, {sum(len(s['message']) for s in encoded)} encoded bytes; nothing sent")
        return 0
    asyncio.run(run_device(opts, encoded))
    return 0


if __name__ == '__main__':
    sys.exit(main())
