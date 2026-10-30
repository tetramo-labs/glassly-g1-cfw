"""Scene frames onto the G1CFW's retained objects, without Bluetooth.

Three layers, all pure Python so tests can run them offline:

- wire builders for the 0xF0 protocol records a STREAM message carries
  (docs/G1_CFW_PROTOCOL.md: SHAPES, retained objects, assets, TEXT);
- the phone's host pipeline between render() and the radio, ported from the
  g2flash shapes suite (anchor resolution, validate / clamp / budget, frame
  diffing) so verdicts mean the same thing here;
- `G1Scene`, one object mirror per temple: every element slot becomes a
  kind-4 shape object (or a text object over a string asset), a frame is one
  OBJ_SHOW, and an animated change becomes an OBJ_ANIM tween the glasses
  render themselves.
"""
import math
import re
import struct
import time

PING, BLIT, FILL, FLUSH, TEXT = 0x00, 0x01, 0x02, 0x03, 0x06
ASSET_PUT, ASSET_CLEAR, ASSET_INFO, SHAPES, STREAM = 0x08, 0x0B, 0x0C, 0x0D, 0x10
OBJ_PUT, OBJ_SHOW, OBJ_HIDE, OBJ_STATE, OBJ_CONTROL, OBJ_ANIM = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25

FRAME_W, FRAME_H = 640, 200
MSG_MAX = 4096
MAX_PACKET = 255
TEXT_MAX = 240
MAX_OBJECTS, MAX_ASSETS = 96, 56
# Objects kept retained before the rest of a frame is drawn directly: headroom under the 96-entry directory.
RETAINED_BUDGET = 90
ANIM_PERIOD_MS = 33
ANIM_MAX_ENTRIES = 16
# Font 0 is the 20 px proportional face on a 27 px pitch; 1 and 2 are the stock digit faces.
FONT_CELL_ROWS = [27, 32, 39]

LINE, RECT, RECT_FILL, CIRCLE, CIRCLE_FILL, TRI, TRI_FILL, QUAD, QUAD_FILL, BEZIER2, BEZIER3, ARC, PIE = range(1, 14)
STROKED = {LINE, RECT, CIRCLE, TRI, QUAD, BEZIER2, BEZIER3, ARC}
# Parameters a tween may target per type, as the firmware enforces.
TWEEN_MASK = {LINE: 0x0F, RECT: 0x1F, RECT_FILL: 0x1F, ARC: 0x1F, PIE: 0x1F, CIRCLE: 0x07, CIRCLE_FILL: 0x07,
              TRI: 0x3F, TRI_FILL: 0x3F, BEZIER2: 0x3F, QUAD: 0xFF, QUAD_FILL: 0xFF, BEZIER3: 0xFF}
# Parameters that are canvas lengths (coordinates, sizes, radii): everything but the ARC/PIE angles.
SHAPE_COORD_MASK = {LINE: 0x0F, RECT: 0x1F, RECT_FILL: 0x1F, ARC: 0x07, PIE: 0x07, CIRCLE: 0x07, CIRCLE_FILL: 0x07,
                    TRI: 0x3F, TRI_FILL: 0x3F, BEZIER2: 0x3F, QUAD: 0xFF, QUAD_FILL: 0xFF, BEZIER3: 0xFF}
# Parameters that are x coordinates (the y is the next one): what a move shifts.
SHAPE_X_PARAMS = {LINE: (0, 2), RECT: (0,), RECT_FILL: (0,), ARC: (0,), PIE: (0,), CIRCLE: (0,), CIRCLE_FILL: (0,),
                  TRI: (0, 2, 4), TRI_FILL: (0, 2, 4), BEZIER2: (0, 2, 4), QUAD: (0, 2, 4, 6), QUAD_FILL: (0, 2, 4, 6), BEZIER3: (0, 2, 4, 6)}
KIND_IMAGE, KIND_TEXT, KIND_RECT, KIND_SHAPES, KIND_SHAPE, KIND_PATH = range(6)
ASSET_1BPP, ASSET_4BPP, ASSET_STRING, ASSET_PATH = range(4)
# OBJ_ANIM value bits beyond the parameters: colour, stroke width, rotation (angle Q4, pivot x, pivot y).
COLOR_BIT, WIDTH_BIT, ROTATE_BITS = 0x100, 0x200, 0x1C00
PATH_TWEEN_MASK = 0x07  # kind 5: x, y, Q8 scale
PATH_SCALE_MAX = 2048
ROTATION_NONE = (0, 0, 0)

SHAPE_TYPES = ['rect', 'circle', 'arc', 'pie', 'line', 'triangle', 'quad', 'bezier']
# What the phone reports for a G1 on the CFW with the vector modes (G1CFW/39 also fills svg paths).
CANVAS = {'width': FRAME_W, 'height': FRAME_H, 'shapes': list(SHAPE_TYPES) + ['svg'], 'animation': True, 'maxTextElements': 120}
MAX_IMAGE_ELEMENTS = 16
# Element fields the pipeline passes through untouched for the encoder.
PASSTHROUGH = ('path', 'pathScale', 'fillRule', 'rotationQ8', 'pivotX', 'pivotY', 'depth', 'fontSize')


# --- wire ------------------------------------------------------------------

def record(cmd, payload):
    return bytes([cmd]) + struct.pack('<H', len(payload)) + payload


def parse_records(message):
    """Split a message back into (cmd, payload) pairs; ValueError when a length does not fit."""
    out, pos = [], 0
    while pos < len(message):
        if pos + 3 > len(message):
            raise ValueError(f'record header runs past the message at {pos}')
        cmd, length = message[pos], struct.unpack_from('<H', message, pos + 1)[0]
        if pos + 3 + length > len(message):
            raise ValueError(f'record {cmd:#x} at {pos} declares {length} bytes past the message')
        out.append((cmd, bytes(message[pos + 3:pos + 3 + length])))
        pos += 3 + length
    return out


def messages(records):
    """Concatenate records into STREAM messages of at most MSG_MAX bytes."""
    out, current = [], b''
    for rec in records:
        if current and len(current) + len(rec) > MSG_MAX:
            out.append(current)
            current = b''
        current += rec
    if current:
        out.append(current)
    return out


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def stream_packets(message, seq=0):
    """The STREAM packets g1flash's StreamSender writes for one message, and the next seq."""
    header = struct.pack('<HH', len(message), crc16(message))
    budget = MAX_PACKET - 4
    chunks, pos, first = [], 0, True
    while first or pos < len(message):
        room = budget - (4 if first else 0)
        chunk = message[pos:pos + room]
        pos += len(chunk)
        chunks.append((header if first else b'') + chunk)
        first = False
    packets = []
    for i, data in enumerate(chunks):
        flags = (0x01 if i == 0 else 0) | (0x06 if i == len(chunks) - 1 else 0)   # last + once (G1CFW/41)
        packets.append(bytes([0xF0, STREAM, seq, flags]) + data)
        seq = (seq + 1) & 0xFF
    return packets, seq


def shape_record(type_, color, width, params, flags=0):
    params = (list(params) + [0] * 8)[:8]
    return bytes([type_, flags, color & 0xF, width & 0xFF]) + struct.pack('<8h', *params)


def shapes_payload(records, push=False):
    return bytes([0x02 if push else 0, len(records)]) + b''.join(records)


def rect_payload(x, y, w, h):
    return struct.pack('<HHHH', x, y, w, h)


def text_payload(font, x0, y0, x1, y1, lines, data, clear=False, push=False):
    flags = (0x01 if clear else 0) | (0x02 if push else 0)
    return bytes([font, flags]) + struct.pack('<HHHH', x0, y0, x1, y1) + bytes([lines]) + data


def asset_put_payload(id_, kind, data, append=False, w=0, h=0):
    return struct.pack('<HBBHH', id_, kind, 1 if append else 0, w, h) + data


def path_asset_payload(id_, commands, rule=0):
    """A path asset: compiled Q4 commands in one record, `w` is the fill rule (0 nonzero, 1 even-odd)."""
    return asset_put_payload(id_, ASSET_PATH, bytes(commands), w=rule)


PATH_COORDS = {0: 2, 1: 2, 2: 4, 3: 6, 4: 0}  # M, L, Q, C, Z: int16 coordinates per command


def path_commands(commands):
    """Compiled path bytes as (op, [Q4 coordinates]) items; ValueError when malformed."""
    out, pos = [], 0
    while pos < len(commands):
        op = commands[pos]
        if op not in PATH_COORDS:
            raise ValueError(f'unknown path command {op} at {pos}')
        n = PATH_COORDS[op]
        if pos + 1 + 2 * n > len(commands):
            raise ValueError(f'path command {op} at {pos} is truncated')
        out.append((op, list(struct.unpack_from(f'<{n}h', commands, pos + 1)) if n else []))
        pos += 1 + 2 * n
    return out


def pack_path_commands(items):
    return b''.join(bytes([op]) + struct.pack(f'<{len(coords)}h', *coords) for op, coords in items)


def path_arena_bytes(commands):
    """What the glasses keep for a path: a 4-byte header and 16 bytes per flattened edge
    (a curve is budgeted at 16 edges)."""
    edges = sum(16 if op in (2, 3) else 1 for op, _ in path_commands(commands) if op != 0)
    return 4 + 16 * edges


def rotation_q4(rotation_q8):
    """The engine's degrees*256 as the firmware's degrees*16, clamped to the int16 the object holds."""
    return max(-32768, min(32767, jround(rotation_q8 / 16)))


def svg_path(d):
    """Compile the SVG `d` subset M L H V Q C S T Z (absolute and relative, implicit
    repetitions) to the firmware's Q4 commands; port of g2flash's svgPath, same
    errors. Every filled contour must close explicitly."""
    number_re = r'[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?'
    tokens = re.findall(r'[a-zA-Z]|' + number_re, d)
    residue = re.sub(r'[a-zA-Z]|' + number_re, '', d)
    if re.search(r'[^\s,]', residue):
        raise ValueError('Invalid SVG path syntax')
    state = {'i': 0}
    out = bytearray()

    def emit(op, *coords):
        out.append(op)
        for v in coords:
            q4 = jround(v * 16)
            if not -32768 <= q4 <= 32767:
                raise ValueError(f'Q4 coordinate outside -32768..32767: {q4}')
            out.extend(struct.pack('<h', q4))

    def number():
        if state['i'] >= len(tokens) or re.fullmatch(r'[a-zA-Z]', tokens[state['i']]):
            raise ValueError('Missing SVG coordinate')
        token = tokens[state['i']]
        state['i'] += 1
        try:
            value = float(token)
        except ValueError:
            raise ValueError('Invalid number') from None
        if not math.isfinite(value):
            raise ValueError('Invalid number')
        return value

    cmd, x, y, sx, sy, is_open, previous = '', 0.0, 0.0, 0.0, 0.0, False, ''
    cx, cy = 0.0, 0.0
    while state['i'] < len(tokens):
        if re.fullmatch(r'[a-zA-Z]', tokens[state['i']]):
            cmd = tokens[state['i']]
            state['i'] += 1
        if not cmd or cmd.upper() not in 'MLHVQCSTZ':
            raise ValueError(f'Unsupported SVG command: {cmd}')
        op, rel = cmd.upper(), cmd != cmd.upper()

        def pair():
            px = number() + (x if rel else 0)
            return px, number() + (y if rel else 0)

        if op == 'Z':
            if not is_open:
                raise ValueError('Z without an open contour')
            emit(4)
            x, y, is_open, previous, cmd = sx, sy, False, 'Z', ''
            continue
        if op != 'M' and not is_open:
            raise ValueError('Path must start with M')
        if op == 'M':
            if is_open:
                raise ValueError('Filled contours must close with Z before another M')
            x, y = pair()
            sx, sy, is_open = x, y, True
            emit(0, x, y)
            cmd = 'l' if rel else 'L'
        elif op in ('L', 'H', 'V'):
            if op == 'L':
                x, y = pair()
            elif op == 'H':
                x = number() + (x if rel else 0)
            else:
                y = number() + (y if rel else 0)
            emit(1, x, y)
        elif op in ('Q', 'T'):
            c = pair() if op == 'Q' else ((2 * x - cx, 2 * y - cy) if previous in ('Q', 'T') else (x, y))
            end = pair()
            emit(2, *c, *end)
            cx, cy = c
            x, y = end
        else:
            a = pair() if op == 'C' else ((2 * x - cx, 2 * y - cy) if previous in ('C', 'S') else (x, y))
            b = pair()
            end = pair()
            emit(3, *a, *b, *end)
            cx, cy = b
            x, y = end
        previous = op
    if is_open or not out:
        raise ValueError('Expected explicitly closed filled path')
    return bytes(out)


def obj_put_payload(id_, version, kind, x, y, w, h, asset=0, level=15, font=0, lines=0, visible=True):
    """Kinds 0-3: image, text, rect, shapes."""
    return struct.pack('<HHBBhhHHHBBB', id_, version, kind, 1 if visible else 0, x, y, w, h, asset, level, font, lines)


def obj_put_shape_payload(id_, version, shape, visible=True):
    """Kind 4: one 20-byte SHAPES record whose parameters, colour and width animate."""
    return struct.pack('<HHBB', id_, version, KIND_SHAPE, 1 if visible else 0) + shape


def obj_show_payload(ids, clear=True, push=True):
    flags = (0x01 if clear else 0) | (0x02 if push else 0)
    return bytes([flags, len(ids)]) + b''.join(struct.pack('<H', i) for i in ids)


def obj_hide_payload(push=True):
    return bytes([0x02 if push else 0])


def obj_control_payload(id_=None):
    return bytes([0]) if id_ is None else bytes([1]) + struct.pack('<H', id_)


EASE_CURVE = 0x80
# CSS cubic-bezier control points x1 y1 x2 y2 scaled to 0..255: g2flash's EASE table, what the glasses take verbatim.
CURVES = {'linear': (0, 0, 255, 255), 'ease-in-out': (107, 0, 148, 255), 'ease-out': (0, 0, 148, 255),
          'ease-in': (107, 0, 255, 255), 'ease': (64, 26, 64, 255)}


def anim_entry(id_, mask, frames, easing, values, curve=None):
    """`[id:u16][mask:u16][frames][easing]` then one i16 per set mask bit, low bit first.
    With a `curve` (four control bytes) the easing byte is 0x80 and the curve follows the values."""
    if len(values) != bin(mask).count('1'):
        raise ValueError(f'{len(values)} values for a mask of {bin(mask).count("1")} bits')
    if curve is not None and len(curve) != 4:
        raise ValueError(f'a cubic-bezier curve is four bytes, not {len(curve)}')
    out = struct.pack('<HHBB', id_, mask, frames, EASE_CURVE if curve is not None else easing) + struct.pack(f'<{len(values)}h', *values)
    return out + bytes(curve) if curve is not None else out


def obj_anim_payload(entries, period_ms=ANIM_PERIOD_MS):
    return bytes([period_ms, len(entries)]) + b''.join(entries)


def utf8_cut(text, limit=TEXT_MAX):
    """UTF-8 bytes cut at a character boundary to fit a string asset."""
    data = text.encode('utf-8')
    if len(data) > limit:
        cut = limit
        while cut > 0 and data[cut] & 0xC0 == 0x80:
            cut -= 1
        data = data[:cut]
    return data


def easing_curve(name):
    """The curve an element transition's easing name rides to the glasses, as the phone
    maps them: None for linear (easing code 0), the CSS `ease` curve for anything unnamed."""
    if name == 'linear':
        return None
    aliases = {'easeOut': 'ease-out', 'easeIn': 'ease-in', 'easeInOut': 'ease-in-out'}
    return CURVES.get(aliases.get(name, name), CURVES['ease'])


def transition_frames(ms):
    if ms <= 0:
        return 0
    return max(1, min(255, jround(ms / ANIM_PERIOD_MS)))


RESET_RECORDS = [record(OBJ_CONTROL, obj_control_payload()), record(ASSET_CLEAR, b'')]


# --- host pipeline (mobile/modules/engine/src/utils/display/scene) -----------

POINT_ARITY = {'line': [2], 'triangle': [3], 'quad': [4], 'bezier': [3, 4]}


def is_num(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def jround(value):
    """Math.round: halves go up, unlike Python's round."""
    return math.floor(value + 0.5)


def js_str(value):
    """String(value) for the values the pipeline hashes."""
    if value is None:
        return 'undefined'
    if value is True:
        return 'true'
    if value is False:
        return 'false'
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def resolve_anchors(elements, width, height):
    out = []
    for el in elements:
        box = el.get('box') if isinstance(el, dict) else None
        if not box or not box.get('anchor'):
            out.append(el)
            continue
        anchor, x, y, w, h = box['anchor'], box['x'], box['y'], box['w'], box['h']
        right = anchor in ('top-right', 'bottom-right')
        bottom = anchor in ('bottom-left', 'bottom-right')
        out.append({**el, 'box': {'x': width - x - w if right else x, 'y': height - y - h if bottom else y, 'w': w, 'h': h}})
    return out


def content_hash(*parts):
    """FNV-1a over the UTF-16 code units of each part (undefined hashes as a space), base 36."""
    h = 0x811c9dc5
    for part in parts:
        s = ' ' if part is None else js_str(part)
        for (unit,) in struct.iter_unpack('<H', s.encode('utf-16-le')):
            h = ((h ^ unit) * 0x01000193) & 0xFFFFFFFF
        h = ((h ^ 0x1f) * 0x01000193) & 0xFFFFFFFF
    digits = '0123456789abcdefghijklmnopqrstuvwxyz'
    out = ''
    while h:
        h, r = divmod(h, 36)
        out = digits[r] + out
    return out or '0'


def element_content_hash(el):
    style = el.get('style') or {}
    style_key = ','.join(f'{k}={js_str(style[k])}' for k in sorted(style))
    points = el.get('points')
    points_key = ';'.join(f'{js_str(p["x"])},{js_str(p["y"])}' for p in points) if points is not None else None
    start, end = el.get('startDeg'), el.get('endDeg')
    angle_key = f'{js_str(start)}-{js_str(end)}' if start is not None or end is not None else None
    return content_hash(el['type'], el.get('text'), el.get('data'), style_key, points_key, angle_key)


def clamp_box(box, width, height):
    x1 = max(0, math.floor(box['x']))
    y1 = max(0, math.floor(box['y']))
    x2 = min(width, math.floor(box['x']) + max(0, math.floor(box['w'])))
    y2 = min(height, math.floor(box['y']) + max(0, math.floor(box['h'])))
    if x2 <= x1 or y2 <= y1:
        return None
    return {'x': x1, 'y': y1, 'w': x2 - x1, 'h': y2 - y1}


def valid_points(el):
    raw = el.get('points')
    if not isinstance(raw, list) or len(raw) not in POINT_ARITY.get(el.get('type'), []):
        return None
    points = []
    for p in raw:
        if not isinstance(p, dict) or not is_num(p.get('x')) or not is_num(p.get('y')):
            return None
        points.append({'x': math.floor(p['x']), 'y': math.floor(p['y'])})
    return points


def points_box(points):
    xs = [p['x'] for p in points]
    ys = [p['y'] for p in points]
    x, y = min(xs), min(ys)
    return {'x': x, 'y': y, 'w': max(xs) - x + 1, 'h': max(ys) - y + 1}


def valid_transition(t):
    if not isinstance(t, dict):
        return None
    duration, easing = t.get('durationMs'), t.get('easing')
    if not is_num(duration) or duration <= 0:
        return None
    return {'durationMs': duration, 'easing': easing} if easing else {'durationMs': duration}


def box_shrunk(orig, clamped):
    return (clamped['x'] != math.floor(orig['x']) or clamped['y'] != math.floor(orig['y'])
            or clamped['w'] != math.floor(orig['w']) or clamped['h'] != math.floor(orig['h']))


def process_scene(elements, caps):
    """Validate, clamp and budget a render() call: {'elements', 'degraded', 'dropped'}."""
    dropped = []
    degraded = False

    def report_id(el, index):
        if isinstance(el, dict) and el.get('id') is not None:
            return el['id']
        kind = el.get('type') if isinstance(el, dict) and el.get('type') is not None else 'text'
        return f'{kind}[{index}]'

    seen = set()
    valid = []
    for index, raw in enumerate(elements):
        if not isinstance(raw, dict):
            dropped.append(report_id(raw, index))
            degraded = True
            continue
        points = None
        if raw.get('type') in POINT_ARITY:
            points = valid_points(raw)
            if points is None:
                dropped.append(report_id(raw, index))
                degraded = True
                continue
            raw = {**raw, 'box': points_box(points)}
        box = raw.get('box')
        if not box:
            dropped.append(report_id(raw, index))
            degraded = True
            continue
        if not all(is_num(box.get(k)) for k in ('x', 'y', 'w', 'h')):
            dropped.append(report_id(raw, index))
            degraded = True
            continue
        if raw.get('type') in ('arc', 'pie') and not (is_num(raw.get('startDeg')) and is_num(raw.get('endDeg'))):
            dropped.append(report_id(raw, index))
            degraded = True
            continue
        el = raw
        if isinstance(raw.get('id'), str) and raw['id'].startswith('~'):
            el = {**raw, 'id': raw['id'].lstrip('~') or None}
        if el.get('id'):
            key = f'{el["type"]}:{el["id"]}'
            if key in seen:
                dropped.append(report_id(el, index))
                degraded = True
                continue
            seen.add(key)
        valid.append((el, index, el['box'], points))

    text_budget = caps['maxTextElements']
    image_budget = MAX_IMAGE_ELEMENTS
    out = []
    for el, index, box, points in valid:
        transition = valid_transition(el.get('transition'))
        clamped = clamp_box(box, caps['width'], caps['height'])
        if clamped is None:
            dropped.append(report_id(el, index))
            degraded = True
            continue
        if box_shrunk(box, clamped):
            degraded = True
        kind = el['type']
        base = {'id': el.get('id'), 'type': kind, 'box': clamped, 'style': el.get('style')}
        for key in PASSTHROUGH:
            if key in el:
                base[key] = el[key]
        if transition:
            base['transition'] = transition
        if kind not in ('text', 'image', 'rect'):
            if kind not in caps['shapes'] or text_budget <= 0:
                dropped.append(report_id(el, index))
                degraded = True
                continue
            text_budget -= 1
            hashed = {'type': kind, 'style': el.get('style'), 'points': points}
            if points is not None:
                base['points'] = points
            if kind in ('arc', 'pie'):
                base['startDeg'], base['endDeg'] = el.get('startDeg'), el.get('endDeg')
                hashed['startDeg'], hashed['endDeg'] = el.get('startDeg'), el.get('endDeg')
            if kind == 'svg':
                hashed['data'] = f"{js_str(el.get('pathScale'))}|{js_str(el.get('fillRule'))}|{bytes(el.get('path') or b'').hex()}"
            out.append({**base, 'contentHash': element_content_hash(hashed)})
            continue
        if kind == 'image':
            # The phone rasterizes these into cached tiles; this port has no rasterizer.
            if not box_shrunk(box, clamped) and image_budget > 0:
                image_budget -= 1
            dropped.append(report_id(el, index))
            degraded = True
            continue
        if text_budget <= 0:
            dropped.append(report_id(el, index))
            degraded = True
            continue
        text_budget -= 1
        if kind == 'rect':
            out.append({**base, 'contentHash': element_content_hash({'type': 'rect', 'style': el.get('style')})})
            continue
        # Text: on the phone the default face is wrapped with the phone's metrics;
        # here every face passes through as given and the glasses clip it.
        text = el.get('text') if el.get('text') is not None else ''
        out.append({**base, 'text': text, 'contentHash': element_content_hash({'type': 'text', 'text': text, 'style': el.get('style')})})
    return {'elements': out, 'degraded': degraded, 'dropped': dropped}


def boxes_equal(a, b):
    return a['x'] == b['x'] and a['y'] == b['y'] and a['w'] == b['w'] and a['h'] == b['h']


def diff_scene(prev, nxt, next_synthetic_id):
    """Match the frame against the previous one: {'elements' (with id and change), 'removed'}."""
    unmatched = list(range(len(prev)))
    by_explicit_id = {f'{el["type"]}:{el["id"]}': i for i, el in enumerate(prev) if not el['id'].startswith('~')}
    matches = [None] * len(nxt)
    for i, el in enumerate(nxt):
        if not el.get('id'):
            continue
        j = by_explicit_id.get(f'{el["type"]}:{el["id"]}')
        if j is not None and j in unmatched:
            matches[i] = j
            unmatched.remove(j)
    for same_box in (True, False):
        for i, el in enumerate(nxt):
            if el.get('id') or matches[i] is not None:
                continue
            for j in unmatched:
                p = prev[j]
                if p['id'].startswith('~') and p['type'] == el['type'] and (not same_box or boxes_equal(p['box'], el['box'])):
                    matches[i] = j
                    unmatched.remove(j)
                    break
    elements = []
    for i, el in enumerate(nxt):
        j = matches[i]
        if j is not None:
            p = prev[j]
            change = 'moved' if not boxes_equal(p['box'], el['box']) else 'updated' if p['contentHash'] != el['contentHash'] else 'unchanged'
            elements.append({**el, 'id': el.get('id') or p['id'], 'change': change})
        else:
            elements.append({**el, 'id': el.get('id') or next_synthetic_id(), 'change': 'created'})
    return {'elements': elements, 'removed': [prev[j]['id'] for j in unmatched]}


def evaluate_result(result, expectation):
    if result['status'] != expectation['status']:
        reason = f" ({result['reason']})" if result.get('reason') else ''
        return {'pass': False, 'reason': f"status {result['status']}{reason}, expected {expectation['status']}"}
    dropped = result.get('dropped') or []
    if dropped != expectation['dropped']:
        return {'pass': False, 'reason': f"dropped [{', '.join(dropped)}], expected [{', '.join(expectation['dropped'])}]"}
    degraded = result.get('degraded') is True
    if degraded != expectation['degraded']:
        return {'pass': False, 'reason': f'degraded {js_str(degraded)}, expected {js_str(expectation["degraded"])}'}
    return {'pass': True, 'reason': f"dropped as expected: {', '.join(dropped)}" if dropped else 'displayed'}


# --- object mirror (G1CfwScene.swift, with kind-4 shape objects) --------------

def clamp_coord(value):
    return max(-1024, min(1023, math.trunc(value)))


def clamp_dim(value):
    return max(0, min(2047, math.trunc(value)))


def idiv(a, b):
    return math.trunc(a / b)


class Slot:
    """One drawable of an element: a kind-4 shape (type, colour, width, p0..p7), a
    kind-1 text object (params x, y, w, h, font, lines over the string bytes) or a
    kind-5 path (type is the fill rule; params x, y, Q8 scale over the command bytes)."""

    def __init__(self, kind, type_, color, width, params, text=b''):
        self.kind = kind
        self.type = type_
        self.color = color
        self.width = width
        self.params = (list(params) + [0] * 8)[:8]
        self.text = text

    def key(self):
        return (self.kind, self.type, self.color, self.width, tuple(self.params), self.text)

    def __eq__(self, other):
        return isinstance(other, Slot) and self.key() == other.key()

    def __ne__(self, other):
        return not self == other

    def record(self):
        return shape_record(self.type, self.color, self.width, self.params)


def shape_slot(type_, color, width, params):
    return Slot(KIND_SHAPE, type_, color, width, params)


def tween_mask(old, new):
    """The OBJ_ANIM mask that takes the slot the glasses show to the next one, or None
    when the change is not a pure geometry / colour / stroke change of the same type.
    The mask covers every tweenable parameter, not just the changed ones: an unmasked
    parameter keeps the object's current value, which is mid-flight while an earlier
    tween runs."""
    if old.kind != new.kind or old.type != new.type or old.text != new.text:
        return None
    if new.kind == KIND_TEXT:
        if old.params[2:] != new.params[2:] or old.color != new.color:
            return None
        return 0x03
    if new.kind == KIND_PATH:
        if old.params[3:] != new.params[3:]:
            return None
        return PATH_TWEEN_MASK | COLOR_BIT
    mask = TWEEN_MASK[new.type]
    for k in range(8):
        if old.params[k] != new.params[k] and not mask & (1 << k):
            return None
    stroked = new.type in STROKED
    if old.width != new.width and not stroked:
        return None
    return mask | COLOR_BIT | (WIDTH_BIT if stroked else 0)


def slot_values(slot, mask, rotation=ROTATION_NONE):
    """The target values for a mask, low bit first: p0..p7, colour, width, angle, pivot x, pivot y."""
    values = list(slot.params) + [slot.color, slot.width] + list(rotation)
    return [values[k] for k in range(13) if mask & (1 << k)]


def tween_entry(id_, old, new, frames, easing=0, curve=None):
    mask = tween_mask(old, new)
    return None if mask is None else anim_entry(id_, mask, frames, easing, slot_values(new, mask), curve)


def rotation_entry(id_, angle_q4, px, py, frames=0, easing=0, curve=None):
    """Rotate a kind-4 or kind-5 object about a canvas pixel; frames 0 snaps."""
    return anim_entry(id_, ROTATE_BITS, frames, easing, [angle_q4, px, py], curve)


def rotation_masks(held, want, frames):
    """How to take an object from the rotation it holds to `want`: (snap mask, glide mask).
    The pivot is a tween value like the angle, so gliding both at once rotates the shape
    about a pivot still on its way (from the origin after a PUT) and swings it off screen.
    While the angle is 0 the pivot is moot, so it is snapped first and only the angle
    glides; a pivot that moves while the shape is already rotated interpolates with it."""
    if frames == 0:
        return ROTATE_BITS, 0
    if held[1:] == want[1:]:
        return 0, 0x400
    if held[0] == 0:
        return 0x1800, 0x400
    return 0, ROTATE_BITS


class Retained:
    """What the glasses hold for one slot of an element."""

    def __init__(self, object_id):
        self.object_id = object_id
        self.version = 0
        self.definition = None   # the slot the resident version defines
        self.state = None        # what the glasses show now; None after a hide mid-tween
        self.rotation = ROTATION_NONE  # (angle Q4, pivot x, pivot y) the glasses hold; a PUT resets it
        self.asset_id = None
        self.asset_bytes = 0
        self.anim_ends_at = 0.0


class Mirror:
    def __init__(self):
        self.retained = {}       # "type:id" -> [Retained], one per slot
        self.free_objects = list(range(MAX_OBJECTS, 0, -1))
        self.free_assets = list(range(MAX_ASSETS, 0, -1))
        self.shown_ids = []      # the list the glasses last composed; empty after a hide
        self.arena_used = 0
        self.newest_asset = None


class ArenaFull(Exception):
    pass


def element_key(el):
    return f'{el["type"]}:{el["id"]}'


class G1Scene:
    """Object mirror for one temple. `encode(frame)` returns the records of one STREAM
    message set (empty when the glasses already show exactly this frame); the caller
    sends them. Ids come from free lists so the first object is 1; elements that
    leave a frame are dropped and their ids reused."""

    def __init__(self, lens_sign=1):
        self.lens_sign = lens_sign
        # What the glasses obtained for assets; 8 KiB is the smallest they settle for.
        self.arena_capacity = 8 * 1024
        self.mirror = Mirror()
        self.ids = []
        self.sets = self.tweens = self.refs = self.deletes = self.directs = 0
        self.anim_ms = 0

    def invalidate(self):
        """Forget everything (a failed send, a reset on the glasses): the next frame redefines what it shows."""
        self.mirror = Mirror()
        self.ids = []

    def resident(self):
        return sum(len(objs) for objs in self.mirror.retained.values())

    def slot_ids(self):
        """Object ids per element of the last frame; empty for elements drawn directly."""
        return self.ids

    def shown_ids(self):
        return list(self.mirror.shown_ids)

    def encode(self, frame):
        if not frame:
            return self.hide()
        try:
            return self._plan(frame)
        except ArenaFull:
            # The estimate says the string arena would overflow: clear the glasses'
            # directory and arena first, then define the whole frame again.
            self.mirror = Mirror()
            return list(RESET_RECORDS) + self._plan(frame, allow_overflow=True)

    def hide(self):
        """The panel goes blank; objects stay. Anything animating right now freezes
        where it is, so it is re-PUT when shown again."""
        now = time.monotonic()
        for objs in self.mirror.retained.values():
            for obj in objs:
                if obj.anim_ends_at > now:
                    obj.state = None
        self.mirror.shown_ids = []
        self.ids = []
        self.sets = self.tweens = self.refs = self.deletes = self.directs = 0
        self.anim_ms = 0
        return [record(OBJ_HIDE, obj_hide_payload(push=True))]

    def _plan(self, frame, allow_overflow=False):
        m = self.mirror
        now = time.monotonic()
        records, directs, snaps, anims, visible, ids = [], [], [], [], [], []
        sets = tweens = refs = deletes = direct = 0
        anim_frames = 0
        changed = False
        live = {element_key(el) for el in frame}
        for key in [k for k in m.retained if k not in live]:
            records += self._drop(key)
            deletes += 1
            changed = True
        for el in frame:
            key = element_key(el)
            dx = self._shift(el)
            slots = self._slots_for(el)
            transition = el.get('transition') or {}
            frames = transition_frames(transition.get('durationMs') or 0)
            curve = easing_curve(transition.get('easing'))
            objs = m.retained.get(key)
            if not slots:
                if objs is not None:
                    records += self._drop(key)
                    deletes += 1
                    changed = True
                ids.append([])
                continue
            if objs is not None and len(objs) > len(slots):
                for obj in objs[len(slots):]:
                    records.append(self._release(obj))
                del objs[len(slots):]
                changed = True
            new = slots[len(objs) if objs is not None else 0:]
            new_assets = sum(1 for s in new if s.kind == KIND_TEXT)
            if new and (self.resident() + len(new) > RETAINED_BUDGET or len(m.free_objects) < len(new)
                        or len(m.free_assets) < new_assets):
                if objs is not None:
                    records += self._drop(key)
                    deletes += 1
                directs += self._direct(slots)
                direct += len(slots)
                changed = True
                ids.append([])
                continue
            if objs is None:
                objs = m.retained[key] = []
            mine = []
            for k, slot in enumerate(slots):
                # One ANIM entry at most per object: a geometry tween, a rotation
                # change, or both in one mask. A PUT resets the rotation on the
                # glasses, so a rotated slot that was PUT snaps it back (frames 0).
                mask, entry_frames = 0, frames
                if k < len(objs):
                    obj = objs[k]
                    if obj.state is not None and obj.state == slot:
                        refs += 1
                    else:
                        mask = tween_mask(obj.state, slot) if obj.state is not None and frames else None
                        if mask is not None:
                            tweens += 1
                            refs += 1
                            obj.state = slot
                        else:
                            mask = 0
                            records += self._put(obj, slot, allow_overflow)
                            sets += 1
                            entry_frames = 0
                        changed = True
                else:
                    obj = Retained(m.free_objects.pop())
                    objs.append(obj)
                    records += self._put(obj, slot, allow_overflow)
                    sets += 1
                    entry_frames = 0
                    changed = True
                rotation = self._rotation(el, dx) if slot.kind in (KIND_SHAPE, KIND_PATH) else ROTATION_NONE
                if rotation[0] == 0:
                    rotation = (0,) + obj.rotation[1:]   # the pivot is moot at angle 0: keep the glasses' value
                if rotation != obj.rotation:
                    snap_mask, glide_mask = rotation_masks(obj.rotation, rotation, entry_frames)
                    if snap_mask and entry_frames:
                        # the pivot lands before the angle starts gliding: its own record, applied first
                        snaps.append(anim_entry(obj.object_id, snap_mask, 0, 0, slot_values(slot, snap_mask, rotation)))
                        snap_mask = 0
                    mask |= snap_mask | glide_mask
                    obj.rotation = rotation
                    changed = True
                if mask:
                    anims.append(anim_entry(obj.object_id, mask, entry_frames, 0, slot_values(slot, mask, rotation), curve if entry_frames else None))
                    if entry_frames:
                        anim_frames = max(anim_frames, entry_frames)
                        obj.anim_ends_at = now + entry_frames * ANIM_PERIOD_MS / 1000
                visible.append(obj.object_id)
                mine.append(obj.object_id)
            ids.append(mine)
        self.ids = ids
        self.sets, self.tweens, self.refs, self.deletes, self.directs = sets, tweens, refs, deletes, direct
        self.anim_ms = anim_frames * ANIM_PERIOD_MS
        if not changed and visible == m.shown_ids:
            return []
        records.append(record(OBJ_SHOW, obj_show_payload(visible, clear=True, push=not directs)))
        if directs:
            records += directs
            records.append(record(FLUSH, rect_payload(0, 0, FRAME_W, FRAME_H)))
        for entries in (snaps, anims):
            for i in range(0, len(entries), ANIM_MAX_ENTRIES):
                records.append(record(OBJ_ANIM, obj_anim_payload(entries[i:i + ANIM_MAX_ENTRIES])))
        m.shown_ids = visible
        return records

    def _drop(self, key):
        return [self._release(obj) for obj in self.mirror.retained.pop(key)]

    def _release(self, obj):
        m = self.mirror
        m.free_objects.append(obj.object_id)
        if obj.asset_id is not None:
            m.free_assets.append(obj.asset_id)
        return record(OBJ_CONTROL, obj_control_payload(obj.object_id))

    def _put(self, obj, slot, allow_overflow):
        """Define or replace the object (version + 1); a text or path slot gets its asset
        first when the bytes (or the fill rule) changed. The text level is not drawn by
        the stock font engine, so it is always 15."""
        m = self.mirror
        out = []
        if slot.kind in (KIND_TEXT, KIND_PATH):
            if obj.asset_id is None:
                obj.asset_id = m.free_assets.pop()
            same = (obj.definition is not None and obj.definition.kind == slot.kind
                    and obj.definition.type == slot.type and obj.definition.text == slot.text)
            if not same:
                size = len(slot.text) if slot.kind == KIND_TEXT else path_arena_bytes(slot.text)
                # Bytes of a replaced asset are only reclaimed when it was the newest.
                reclaim = obj.asset_bytes if m.newest_asset == obj.asset_id else 0
                if not allow_overflow and m.arena_used - reclaim + size > self.arena_capacity:
                    raise ArenaFull()
                m.arena_used += size - reclaim
                m.newest_asset = obj.asset_id
                obj.asset_bytes = size
                if slot.kind == KIND_TEXT:
                    out.append(record(ASSET_PUT, asset_put_payload(obj.asset_id, ASSET_STRING, slot.text)))
                else:
                    out.append(record(ASSET_PUT, path_asset_payload(obj.asset_id, slot.text, slot.type)))
            obj.version = (obj.version + 1) & 0xFFFF or 1
            if slot.kind == KIND_TEXT:
                x, y, w, h, font, lines = slot.params[:6]
                out.append(record(OBJ_PUT, obj_put_payload(obj.object_id, obj.version, KIND_TEXT, x, y, w, h, obj.asset_id, 15, font, lines)))
            else:
                x, y, scale = slot.params[:3]
                out.append(record(OBJ_PUT, obj_put_payload(obj.object_id, obj.version, KIND_PATH, x, y, scale, 0, obj.asset_id, slot.color)))
        else:
            if obj.asset_id is not None:
                m.free_assets.append(obj.asset_id)
                obj.asset_id = None
                obj.asset_bytes = 0
            obj.version = (obj.version + 1) & 0xFFFF or 1
            out.append(record(OBJ_PUT, obj_put_shape_payload(obj.object_id, obj.version, slot.record())))
        obj.definition = obj.state = slot
        obj.rotation = ROTATION_NONE
        obj.anim_ends_at = 0
        return out

    def _direct(self, slots):
        """Records that draw the slots straight into the frame (after the SHOW). A path
        has no direct record (and nothing direct rotates), so those slots are left out."""
        out, shapes = [], []

        def flush_shapes():
            if shapes:
                out.append(record(SHAPES, shapes_payload(shapes)))
                shapes.clear()

        for slot in slots:
            if slot.kind == KIND_TEXT:
                flush_shapes()
                x, y, w, h, font, lines = slot.params[:6]
                out.append(record(TEXT, text_payload(font, x, y, x + w, y + h - 1, lines, slot.text)))
            elif slot.kind == KIND_SHAPE:
                shapes.append(slot.record())
        flush_shapes()
        return out

    def _shift(self, el):
        """An element's `depth` moves it this many columns on this lens."""
        return math.trunc(el.get('depth') or 0) * self.lens_sign

    def _rotation(self, el, dx):
        """(angle Q4, pivot x, pivot y) for an element's `rotationQ8` and `pivotX`/`pivotY`
        (canvas pixels, default the box centre); ROTATION_NONE when it is not rotated."""
        angle = rotation_q4(el.get('rotationQ8') or 0)
        if angle == 0:
            return ROTATION_NONE
        box = el['box']
        px = el.get('pivotX')
        py = el.get('pivotY')
        if px is None:
            px = box['x'] + idiv(math.trunc(box['w']), 2)
        if py is None:
            py = box['y'] + idiv(math.trunc(box['h']), 2)
        return (angle, clamp_coord(px + dx), clamp_coord(py))

    def _slots_for(self, el):
        style = el.get('style') or {}
        color = style.get('color')
        gray = 15 if color is None or color < 0 else max(0, min(15, math.trunc(color)))
        # Open strokes carry their width as `width`; closed shapes and boxes as `border`.
        border = style.get('border')
        if border is None:
            border = style.get('width')
        border = math.trunc(border or 0)
        stroke = max(1, min(255, border))
        dx = self._shift(el)
        box = el['box']
        x, y = clamp_coord(box['x'] + dx), clamp_coord(box['y'])
        w, h = clamp_dim(box['w']), clamp_dim(box['h'])
        radius = clamp_dim(style.get('radius') or 0)
        points = el.get('points') or []

        def pt(i):
            if i < len(points):
                return [clamp_coord(points[i]['x'] + dx), clamp_coord(points[i]['y'])]
            return [0, 0]

        def circle():
            bw, bh = math.trunc(box['w']), math.trunc(box['h'])
            r = max(0, idiv(min(bw, bh) - 1, 2))
            return [clamp_coord(box['x'] + idiv(bw, 2) + dx), clamp_coord(box['y'] + idiv(bh, 2)), r]

        kind = el['type']
        if kind == 'rect':
            if style.get('fill'):
                out = [shape_slot(RECT_FILL, gray, 0, [x, y, w, h, radius])]
                if border > 0:
                    out.append(shape_slot(RECT, gray, stroke, [x, y, w, h, radius]))
                return out
            return [shape_slot(RECT, gray, stroke, [x, y, w, h, radius])]
        if kind == 'circle':
            cx, cy, r = circle()
            if style.get('fill'):
                return [shape_slot(CIRCLE_FILL, gray, 0, [cx, cy, r])]
            return [shape_slot(CIRCLE, gray, stroke, [cx, cy, r])]
        if kind in ('arc', 'pie'):
            cx, cy, r = circle()
            a0, a1 = clamp_coord(el.get('startDeg') or 0), clamp_coord(el.get('endDeg') or 0)
            if kind == 'pie':
                return [shape_slot(PIE, gray, 0, [cx, cy, r, a0, a1])]
            return [shape_slot(ARC, gray, stroke, [cx, cy, r, a0, a1])]
        if kind == 'line':
            return [shape_slot(LINE, gray, stroke, pt(0) + pt(1))]
        if kind in ('triangle', 'quad'):
            n = 3 if kind == 'triangle' else 4
            params = [v for i in range(n) for v in pt(i)]
            if style.get('fill'):
                return [shape_slot(TRI_FILL if n == 3 else QUAD_FILL, gray, 0, params)]
            return [shape_slot(TRI if n == 3 else QUAD, gray, stroke, params)]
        if kind == 'bezier':
            n = len(points)
            if n not in (3, 4):
                return []
            params = [v for i in range(n) for v in pt(i)]
            return [shape_slot(BEZIER2 if n == 3 else BEZIER3, gray, stroke, params)]
        if kind == 'text':
            out = []
            if border > 0:
                out.append(shape_slot(RECT, gray, stroke, [x, y, w, h, radius]))
            slot = self._text_slot(el, dx)
            if slot is not None:
                out.append(slot)
            return out
        if kind == 'svg':
            # A compiled path (the engine's svgPath output) filled at the box origin.
            commands = bytes(el.get('path') or b'')
            if not commands:
                return []
            scale = el.get('pathScale')
            scale = max(0, min(PATH_SCALE_MAX, math.trunc(256 if scale is None else scale)))
            rule = 1 if el.get('fillRule') in (1, 'evenodd') else 0
            return [Slot(KIND_PATH, rule, gray, 0, [x, y, scale], commands)]
        return []

    def _text_slot(self, el, dx):
        """The kind-1 object for a text element: stock font engine rules (even x
        bounds, the cell must fit under row 200, one line per 27 rows for font 0)."""
        text = el.get('text') or ''
        if not text:
            return None
        style = el.get('style') or {}
        size = style.get('fontSize', el.get('fontSize')) or 0
        numeric = all(ch.isnumeric() or ch in ':.% ' for ch in text)
        font = 2 if numeric and size >= 36 else 1 if numeric and size >= 30 else 0
        cell = FONT_CELL_ROWS[font]
        box = el['box']
        x = math.trunc(box['x']) + dx
        x1 = x + math.trunc(box['w'])
        x = max(0, x) & ~1
        x1 = min(FRAME_W, x1) & ~1
        if x1 - x < 2:
            return None
        y = max(0, math.trunc(box['y']))
        h = min(math.trunc(box['h']), 201 - cell - y)
        if h < 1:
            return None
        lines = max(1, h // FONT_CELL_ROWS[0]) if font == 0 else 1
        return Slot(KIND_TEXT, 0, 15, 0, [x, y, x1 - x, h, font, lines], utf8_cut(text))
