#!/usr/bin/env python3
"""G1-only offline planning, inspection, staging, activation, and stock restore."""
import argparse
import asyncio
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import uuid

from build_cfw import PROFILES, PUBLIC_KEY, ROOT, load_profiles
from cfw.image import FirmwareError
from cfw.plan import plan_image
from cfw.smp import CHUNK_BYTES, NUS_RX, NUS_TX, SMP, activate, confirm, upload


def printable(value):
    if isinstance(value, bytes):
        return value.hex()
    raise TypeError(type(value).__name__)


def emit(value):
    print(json.dumps(value, default=printable, indent=2), flush=True)


class Journal:
    def __init__(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        # Never overwrite evidence from a prior attempt.
        self.file = path.open('x')

    def event(self, name, details):
        row = {'time': datetime.now(timezone.utc).isoformat(), 'event': name, 'details': details}
        self.file.write(json.dumps(row, default=printable) + '\n')
        self.file.flush()
        os.fsync(self.file.fileno())

    def close(self):
        self.file.close()


def is_g1(name, side=None):
    match = re.fullmatch(r'Even G1(?:V\d+)?(?:_\d+)?_([LR])_[0-9A-Fa-f]{6}', name or '')
    return bool(match and (side is None or match[1] == side))


def diagnostic(reply):
    # A complete diagnostic line is required, not an arbitrary marker substring.
    match = re.search(rb'(?:^|[\r\n\x00])(?P<marker>G1CFW/\d+ net ?bt:|net build time:) '
                      rb'[^\x00\r\n]+, app build time (?P<build>[^\x00\r\n]+), '
                      rb'ver (?P<version>\d+\.\d+\.\d+), JBD DeviceID [0-9a-fA-F]+\r?\n', reply)
    if not match:
        raise FirmwareError('Incomplete/unrecognized G1 diagnostic reply')
    return {'cfw_marker': match['marker'].startswith(b'G1CFW/'),
            'build': match['build'].decode(), 'version': match['version'].decode()}


def verify_reply(reply, expected):
    result = diagnostic(reply)
    if result['cfw_marker'] != (expected == 'cfw'):
        raise FirmwareError(f'Expected {expected} diagnostic marker was not observed')
    if (result['version'], result['build']) not in {
        ('1.5.60', '2025-03-21 11:38:43'), ('1.6.3', '2025-06-28 17:28:42')
    }:
        raise FirmwareError('Diagnostic build/version does not match the pinned base')
    return result


async def query_marker(client, timeout=8):
    if not client.services.get_characteristic(NUS_RX) or not client.services.get_characteristic(NUS_TX):
        raise FirmwareError('G1 Nordic UART service absent')
    received = bytearray()
    done = asyncio.Event()
    overflow = False

    def on_data(_sender, data):
        nonlocal overflow
        if len(received) + len(data) > 4096:
            overflow = True
            done.set()
            return
        received.extend(data)
        try:
            diagnostic(bytes(received))
        except FirmwareError:
            return
        done.set()

    await client.start_notify(NUS_TX, on_data)
    try:
        characteristic = client.services.get_characteristic(NUS_RX)
        await client.write_gatt_char(characteristic, b'#t', response='write' in characteristic.properties)
        await asyncio.wait_for(done.wait(), timeout)
        if overflow:
            raise FirmwareError('Diagnostic response exceeded 4096 bytes')
    finally:
        await client.stop_notify(NUS_TX)
    return bytes(received)


async def query_ping(client, timeout=8):
    if not client.services.get_characteristic(NUS_RX) or not client.services.get_characteristic(NUS_TX):
        raise FirmwareError('G1 Nordic UART service absent')
    received = []
    done = asyncio.Event()

    def on_data(_sender, data):
        if bytes(data[:1]) == b'\xf0':
            received.append(bytes(data))
            done.set()

    await client.start_notify(NUS_TX, on_data)
    try:
        characteristic = client.services.get_characteristic(NUS_RX)
        await client.write_gatt_char(characteristic, b'\xf0\x00', response='write' in characteristic.properties)
        await asyncio.wait_for(done.wait(), timeout)
    finally:
        await client.stop_notify(NUS_TX)
    reply = received[0]
    end = reply.find(b'\x00', 3)
    if len(reply) < 11 or reply[1:3] != b'\x00\x00' or reply[3:8] != b'G1CFW' or end < 0 or len(reply) < end + 4:
        raise FirmwareError(f'Unexpected ping reply: {reply.hex()}')
    return {'reply_hex': reply.hex(), 'identity': reply[3:end].decode('ascii', 'replace'),
            'base_version': '.'.join(str(b) for b in reply[end + 1:end + 4])}


CFW_SID = 0xF0
CFW_PING, CFW_BLIT, CFW_FILL, CFW_FLUSH, CFW_INFO, CFW_PANEL, CFW_TEXT, CFW_COPY = 0, 1, 2, 3, 4, 5, 6, 7
CFW_ASSET_PUT, CFW_ASSET_DRAW, CFW_ASSET_TEXT, CFW_ASSET_CLEAR, CFW_ASSET_INFO, CFW_SHAPES, CFW_STREAM, CFW_STATS, CFW_PEEK = 8, 9, 10, 11, 12, 13, 0x10, 0x11, 0x12
CFW_OBJ_PUT, CFW_OBJ_SHOW, CFW_OBJ_HIDE, CFW_OBJ_STATE, CFW_OBJ_CONTROL, CFW_OBJ_ANIM, CFW_ANIM_STATE = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26
CFW_INPUT_CONTROL, CFW_INPUT_STATE, CFW_INPUT_EVENT = 0x30, 0x31, 0x32
CFW_VIEW, CFW_IMU_CONTROL, CFW_IMU_SAMPLE = 0x14, 0x33, 0x34
CFW_MIC_CONTROL = 0x40
UNSOLICITED = {0x32, 0x34}   # INPUT_EVENT, IMU_SAMPLE: never a reply to a request
STOCK_AUDIO_SID = 0xF1
DEPTH_SHIFT = {1: 22, 2: 8, 3: 0, 4: -4, 5: -8, 6: -10, 7: -12, 8: -14, 9: -16}  # stock cal_panel_canvas_coord, non-master sign
# Raw diagnostic reads must follow the base reported by PING. GlassesState is
# allocated by stock firmware: resolve its pointer instead of assuming a heap address.
STOCK_LAYOUTS = {
    (1, 5, 60): {'state_ptr': 0x20006b9c, 'wear_offset': 0x109d,
                 'wear_word': 0x20006b88, 'imu': 0x20007c40},
    (1, 6, 3): {'state_ptr': 0x20006bdc, 'wear_offset': 0x10d4,
                'wear_word': 0x20006bc8, 'imu': 0x20008484},
}
TOUCH_CODES = {1: 'single', 2: 'double', 3: 'triple', 4: 'long-start', 5: 'long-end', 6: 'long-timeout'}
STOCK_EVENTS = {2: 'head-up', 3: 'head-down', 6: 'worn', 7: 'taken-off', 8: 'in-case-open', 9: 'charging',
                10: 'battery', 0x0b: 'in-case-closed', 0x0e: 'case-charging', 0x0f: 'case-battery', 0x11: 'bind-finished'}
WORK_MODES = {0: 'boot', 1: 'link-setup', 2: 'normal-run', 3: 'normal-standby', 4: 'normal-low-power',
              5: 'charging-run', 6: 'charging-low-power', 7: 'ship', 8: 'deep-sleep', 9: 'waking'}
EVENT_CLASSES = {0: ('touch', TOUCH_CODES), 1: ('stock', STOCK_EVENTS)}
CFW_STATUS_PANEL_OFF = 4
CFW_STATUS = {0: 'ok', 1: 'bad-length', 2: 'bad-command', 3: 'bad-rect', 4: 'panel-off', 5: 'bad-seq',
              6: 'bad-crc', 7: 'too-big', 8: 'bad-record', 9: 'bad-rle', 10: 'bad-font', 11: 'no-memory',
              12: 'no-asset', 13: 'bad-asset', 14: 'no-object'}
CFW_MSG_MAX = 4096
FRAME_W, FRAME_H, MAX_PACKET = 640, 200, 255


class CfwLink:
    """Request/reply helper for the private 0xF0 protocol over the Nordic UART service."""

    def __init__(self, client):
        self.client = client
        self.rx = client.services.get_characteristic(NUS_RX)
        self.replies = asyncio.Queue()
        self.audio = asyncio.Queue()   # stock [F1][seq][LC3 chunk] notifications
        self.events = asyncio.Queue()  # unsolicited F0 packets (INPUT_EVENT, IMU_SAMPLE)

    def on_notify(self, _sender, data):
        data = bytes(data)
        if data[:1] == b'\xf0':
            (self.events if len(data) >= 2 and data[1] in UNSOLICITED else self.replies).put_nowait(data)
        elif data[:1] == bytes([STOCK_AUDIO_SID]):
            self.audio.put_nowait(data)

    async def __aenter__(self):
        if not self.rx or not self.client.services.get_characteristic(NUS_TX):
            raise FirmwareError('G1 Nordic UART service absent')
        await self.client.start_notify(NUS_TX, self.on_notify)
        return self

    async def __aexit__(self, *_):
        await self.client.stop_notify(NUS_TX)

    async def request(self, sub, payload=b'', timeout=8):
        packet = bytes([CFW_SID, sub]) + payload
        if len(packet) > MAX_PACKET:
            raise FirmwareError(f'CFW packet of {len(packet)} bytes exceeds the stock receive buffer')
        await self.client.write_gatt_char(self.rx, packet, response='write' in self.rx.properties)
        while True:
            reply = await asyncio.wait_for(self.replies.get(), timeout)
            if len(reply) >= 3 and reply[1] == sub:
                return reply[2], reply[3:]

    async def check(self, sub, payload=b''):
        status, body = await self.request(sub, payload)
        if status != 0:
            raise FirmwareError(f'CFW command {sub:#x} failed: {CFW_STATUS.get(status, status)}')
        return body

    async def stock_layout(self):
        body = await self.check(CFW_PING)
        marker, sep, version = body.partition(b'\0')
        if not sep or not re.fullmatch(rb'G1CFW/\d+', marker) or len(version) != 3:
            raise FirmwareError('Invalid CFW PING base identity')
        layout = STOCK_LAYOUTS.get(tuple(version))
        if layout is None:
            raise FirmwareError(f'Unsupported CFW base for RAM diagnostics: {tuple(version)}')
        return layout

    async def wear_state(self):
        layout = await self.stock_layout()
        pointer = await self.check(CFW_PEEK, struct.pack('<IB', layout['state_ptr'], 4))
        if len(pointer) != 4:
            raise FirmwareError('Incomplete GlassesState pointer')
        state = struct.unpack('<I', pointer)[0]
        if state % 4 or not 0x20000000 <= state < 0x20080000 - layout['wear_offset']:
            raise FirmwareError('Invalid GlassesState RAM pointer')
        word = await self.check(CFW_PEEK, struct.pack('<IB', layout['wear_word'], 4))
        detect = await self.check(CFW_PEEK, struct.pack('<IB', state + layout['wear_offset'], 1))
        if len(word) != 4 or len(detect) != 1:
            raise FirmwareError('Incomplete wear diagnostics')
        return {'wear_word': struct.unpack('<I', word)[0], 'wear_detect': bool(detect[0])}


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def packbits(data):
    """PackBits: runs of 3+ equal bytes become [257-n][byte]; others are literals."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes([257 - run, data[i]])
            i += run
            continue
        start = i
        while i < n and i - start < 128:
            run = 1
            while i + run < n and run < 3 and data[i + run] == data[i]:
                run += 1
            if run >= 3:
                break
            i += 1
        out += bytes([i - start - 1]) + data[start:i]
    return bytes(out)


def record(cmd, payload):
    return bytes([cmd]) + struct.pack('<H', len(payload)) + payload


def shape_record(kind, color, width, *params):
    """One 20-byte SHAPES record (G1CFW/38): [type][flags][color][width][p0..p7 as int16]."""
    params = list(params) + [0] * (8 - len(params))
    return bytes([kind, 0, color & 0xF, width & 0xFF]) + struct.pack('<8h', *params)


def anim_entry(oid, mask, frames, easing, *values):
    """One OBJ_ANIM tween entry: [id][mask][frames][easing] then an int16 target per set mask bit."""
    if bin(mask).count('1') != len(values):
        raise FirmwareError(f'tween mask {mask:#x} needs {bin(mask).count("1")} values, got {len(values)}')
    return struct.pack('<HHBB', oid, mask, frames, easing) + struct.pack(f'<{len(values)}h', *values)


class StreamSender:
    """Pipelined multi-packet messages: packets go out without waiting; only
    the last packet of a message is answered."""

    def __init__(self, link):
        self.link = link
        self.seq = 0
        self.packets = 0
        self.deferred = False  # a flush met the panel off: pixels wait in the frame buffer

    async def send(self, message, timeout=10):
        if len(message) > CFW_MSG_MAX:
            raise FirmwareError(f'message of {len(message)} bytes exceeds {CFW_MSG_MAX}')
        header = struct.pack('<HH', len(message), crc16(message))
        budget = MAX_PACKET - 4  # F0 10 seq flags
        chunks, pos, first = [], 0, True
        while first or pos < len(message):
            room = budget - (4 if first else 0)
            chunk = message[pos:pos + room]
            pos += len(chunk)
            chunks.append((first, (header if first else b'') + chunk))
            first = False
        first_seq = self.seq
        for i, (is_first, data) in enumerate(chunks):
            # Bit 2 (G1CFW/41): a repeat of the message that ran last is acknowledged, not re-run.
            flags = (0x01 if is_first else 0) | (0x06 if i == len(chunks) - 1 else 0)
            packet = bytes([CFW_SID, CFW_STREAM, self.seq, flags]) + data
            await self.link.client.write_gatt_char(self.link.rx, packet, response=False)
            self.seq = (self.seq + 1) & 0xFF
            self.packets += 1
        last_seq = (self.seq - 1) & 0xFF
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                raise TimeoutError('Timed out waiting for STREAM acknowledgement')
            reply = await asyncio.wait_for(self.link.replies.get(), remaining)
            if len(reply) >= 5 and reply[1] == CFW_STREAM:
                status, seq, done = reply[2], reply[3], reply[4]
                # Only the final packet can acknowledge execution; errors may
                # refer to any packet in this message, including across wrap.
                if ((seq - first_seq) & 0xFF) >= len(chunks):
                    continue
                if status in (0, CFW_STATUS_PANEL_OFF) and seq != last_seq:
                    continue
                if status == 4:
                    self.deferred = True   # G1CFW/32+: every record ran, the panel is off
                elif status != 0:
                    raise FirmwareError(f'stream message failed: {CFW_STATUS.get(status, status)} at seq {seq}, {done} records done')
                return done


def rect_bytes(x, y, w, h):
    if x % 2 or w % 2 or w <= 0 or h <= 0 or x + w > FRAME_W or y + h > FRAME_H:
        raise FirmwareError('Rectangle must use even x/width and fit in 640x200')
    return struct.pack('<HHHH', x, y, w, h)


def blit_packets(x, y, width, rows, level=15, flush=True):
    """Split 1bpp rows (list of bytes, MSB-first, padded) into CFW_BLIT payloads."""
    stride = (width + 7) // 8
    flags = 0x01 | (0x02 if flush else 0) | ((level & 0xF) << 4)
    per_packet = max(1, (MAX_PACKET - 2 - 9) // stride)
    for start in range(0, len(rows), per_packet):
        chunk = rows[start:start + per_packet]
        yield bytes([flags]) + rect_bytes(x, y + start, width, len(chunk)) + b''.join(chunk)


def test_pattern_rows():
    rows = []
    for y in range(FRAME_H):
        bits = []
        for x in range(FRAME_W):
            border = x < 2 or x >= FRAME_W - 2 or y < 2 or y >= FRAME_H - 2
            checker = ((x // 20) + (y // 20)) % 2 == 0 and 40 <= x < 280 and 40 <= y < 160
            diagonal = abs((x - 320) - (y - 100) * 3) < 3 and x >= 320
            stripes = x >= 320 and (y // 10) % 2 == 0 and y >= 150
            bits.append(border or checker or diagonal or stripes)
        row = bytearray((FRAME_W + 7) // 8)
        for x, on in enumerate(bits):
            if on:
                row[x // 8] |= 0x80 >> (x % 8)
        rows.append(bytes(row))
    return rows


def read_pbm(path):
    data = path.read_bytes()
    if not data.startswith(b'P4'):
        raise FirmwareError('Only binary PBM (P4) images are supported')
    fields, pos = [], 2
    while len(fields) < 2:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(int(data[pos:end]))
        pos = end
    pos += 1
    width, height = fields
    stride = (width + 7) // 8
    rows = [data[pos + i * stride:pos + (i + 1) * stride] for i in range(height)]
    if len(rows[-1]) != stride:
        raise FirmwareError('Truncated PBM')
    return width, height, rows


async def run_cfw_command(client, args, checkpoint):
    async with CfwLink(client) as link:
        if args.command == 'info':
            body = await link.check(CFW_INFO)
            if len(body) < 12:
                raise FirmwareError('Short INFO reply')
            result = {'panel_powered': bool(body[0]), 'max_packet': body[1],
                      'col_offset': struct.unpack_from('<h', body, 2)[0], 'row_offset': struct.unpack_from('<h', body, 4)[0],
                      'frame_buffer': hex(struct.unpack_from('<I', body, 6)[0]), 'width': body[10] * 4, 'height': body[11]}
        elif args.command == 'panel':
            mode = {'on': 1, 'off': 0, 'force_on': 2, 'force_off': 3, 'resume': 5, 'suspend': 4, 'wake': 7, 'sleep': 6}
            requested = next(name for name in mode if getattr(args, name))
            body = await link.check(CFW_PANEL, bytes([mode[requested]]))
            result = {'requested': requested, 'power_state': body[0], 'panel_powered': bool(body[1])}
        elif args.command == 'fill':
            x, y, w, h = args.rect
            flags = 0x00 if args.no_flush else 0x02
            await link.check(CFW_FILL, bytes([flags]) + rect_bytes(x, y, w, h) + bytes([args.level & 0xF]))
            result = {'filled': args.rect, 'level': args.level, 'flushed': not args.no_flush}
        elif args.command == 'text':
            x0, y0, x1, y1 = args.box
            flags = (0x01 if args.clear else 0) | (0 if args.no_flush else 0x02)
            payload = bytes([args.font, flags]) + struct.pack('<HHHH', x0, y0, x1, y1) + bytes([args.lines]) + args.text.encode('utf-8')
            await link.check(CFW_TEXT, payload)
            result = {'text': args.text, 'font': args.font, 'box': args.box, 'lines': args.lines, 'flushed': not args.no_flush}
        elif args.command == 'asset':
            if args.action == 'info':
                body = await link.check(CFW_ASSET_INFO)
                result = {'assets': body[0], 'max_assets': body[1], 'used_bytes': body[2] | body[3] << 8 | body[4] << 16,
                          'arena_kib': body[5], 'arena_allocated': bool(body[6])}
            elif args.action == 'clear':
                await link.check(CFW_ASSET_CLEAR)
                result = {'cleared': True}
            elif args.action == 'put':
                if args.string is not None:
                    kind, w, h, data = 2, 0, 0, args.string.encode('utf-8')
                else:
                    w, h, rows = read_pbm(args.pbm) if args.pbm else (FRAME_W, FRAME_H, test_pattern_rows())
                    kind, data = 0, b''.join(rows)
                sender = StreamSender(link)
                records, first = [], True
                step = CFW_MSG_MAX - 64
                for pos in range(0, max(len(data), 1), step):
                    chunk = data[pos:pos + step]
                    records.append(record(CFW_ASSET_PUT, struct.pack('<HBBHH', args.id, kind, 0 if first else 1, w, h) + chunk))
                    first = False
                for rec in records:
                    await sender.send(rec)
                result = {'id': args.id, 'kind': kind, 'bytes': len(data), 'messages': len(records)}
            elif args.action == 'draw':
                flags = (0 if args.no_flush else 0x02) | ((args.level & 0xF) << 4)
                await link.check(CFW_ASSET_DRAW, struct.pack('<HBHH', args.id, flags, args.at[0], args.at[1]))
                result = {'id': args.id, 'at': args.at, 'flushed': not args.no_flush}
            else:
                x0, y0, x1, y1 = args.box
                flags = (0x01 if args.clear else 0) | (0 if args.no_flush else 0x02)
                await link.check(CFW_ASSET_TEXT, struct.pack('<HBBHHHHB', args.id, args.font, flags, x0, y0, x1, y1, args.lines))
                result = {'id': args.id, 'font': args.font, 'box': args.box}
        elif args.command == 'shapes':
            if args.demo:
                recs = [shape_record(2, 15, 2, 4, 4, 632, 192, 12),          # rounded frame
                        shape_record(5, 15, 0, 120, 100, 60),               # disc
                        shape_record(4, 8, 4, 120, 100, 80),                # ring
                        shape_record(1, 15, 3, 240, 20, 620, 180),          # 3 px line
                        shape_record(1, 15, 1, 240, 180, 620, 20),          # hairline
                        shape_record(7, 10, 0, 430, 30, 520, 30, 475, 110), # filled triangle
                        shape_record(6, 15, 2, 300, 150, 420, 150, 360, 60),
                        shape_record(3, 12, 0, 240, 70, 100, 40, 14),       # rounded fill
                        shape_record(12, 15, 4, 560, 100, 40, 135, 405),    # gauge arc
                        shape_record(13, 9, 0, 560, 100, 25, -90, 135),     # sector
                        shape_record(11, 15, 2, 250, 140, 320, 90, 400, 190, 470, 140)]
            else:
                recs = []
                for spec in args.shape:
                    kind, color, width, *pts = (int(v) for v in spec.split(','))
                    recs.append(shape_record(kind, color, width, *pts))
            payload = bytes([0 if args.no_flush else 0x02, len(recs)]) + b''.join(recs)
            await link.check(CFW_SHAPES, payload)
            result = {'shapes': len(recs), 'flushed': not args.no_flush}
        elif args.command == 'obj':
            kinds = {'image': 0, 'text': 1, 'rect': 2, 'shapes': 3, 'shape': 4}
            if args.action == 'put':
                if args.id is None:
                    raise FirmwareError('obj put needs --id')
                if args.kind == 'shape':
                    if not args.record:
                        raise FirmwareError('obj put --kind shape needs --record type,color,width,p0,...')
                    kind, color, width, *pts = (int(v) for v in args.record.split(','))
                    payload = struct.pack('<HHBB', args.id, args.version, 4, 1 if not args.hidden else 0) + shape_record(kind, color, width, *pts)
                else:
                    payload = struct.pack('<HHBBhhHHHBBB', args.id, args.version, kinds[args.kind], 1 if not args.hidden else 0,
                                          args.at[0], args.at[1], args.size[0], args.size[1], args.asset, args.level, args.font, args.lines)
                await link.check(CFW_OBJ_PUT, payload)
                result = {'id': args.id, 'kind': args.kind, 'at': args.at}
            elif args.action == 'show':
                ids = [int(v) for v in args.ids.split(',')] if args.ids else []
                flags = (0x01 if args.clear else 0) | (0 if args.no_flush else 0x02)
                await link.check(CFW_OBJ_SHOW, bytes([flags, len(ids)]) + b''.join(struct.pack('<H', i) for i in ids))
                result = {'shown': ids}
            elif args.action == 'hide':
                await link.check(CFW_OBJ_HIDE, bytes([0 if args.no_flush else 0x02]))
                result = {'hidden': True}
            elif args.action == 'state':
                body = await link.check(CFW_OBJ_STATE)
                shown = [(struct.unpack_from('<H', body, 4 + 4 * i)[0], struct.unpack_from('<H', body, 6 + 4 * i)[0]) for i in range((len(body) - 4) // 4)]
                result = {'revision': struct.unpack_from('<H', body)[0], 'objects': body[2], 'shown_count': body[3], 'shown': shown}
            elif args.action == 'anim':
                moves = []
                for spec in args.move:
                    oid, target, rest = spec.split(':', 2) if spec.count(':') >= 2 else (*spec.split(':', 1), '8')
                    tx, ty = (int(v) for v in target.split(','))
                    frames, _, ease = rest.partition(':')
                    moves.append(anim_entry(int(oid), 0x03, int(frames), int(ease or 0), tx, ty))
                for spec in args.tween:
                    oid, mask, frames, ease, values = spec.split(':', 4)
                    moves.append(anim_entry(int(oid), int(mask, 0), int(frames), int(ease), *(int(v) for v in values.split(','))))
                before = await link.check(CFW_ANIM_STATE)
                started = asyncio.get_running_loop().time()
                await link.check(CFW_OBJ_ANIM, bytes([args.period, len(moves)]) + b''.join(moves))
                result = {'moves': len(moves), 'period_ms': args.period}
                if args.wait:
                    while True:
                        await asyncio.sleep(0.05)
                        body = await link.check(CFW_ANIM_STATE)
                        if not body[0]:
                            break
                    elapsed = asyncio.get_running_loop().time() - started
                    frames = struct.unpack_from('<I', body, 2)[0] - struct.unpack_from('<I', before, 2)[0]
                    result.update({'frames': frames, 'seconds': round(elapsed, 2), 'fps': round(frames / elapsed, 1) if elapsed else None,
                                   'full_pushes': struct.unpack_from('<I', body, 6)[0] - struct.unpack_from('<I', before, 6)[0]})
            else:
                payload = bytes([0]) if args.id is None else bytes([1]) + struct.pack('<H', args.id)
                await link.check(CFW_OBJ_CONTROL, payload)
                result = {'dropped': 'all' if args.id is None else args.id}
        elif args.command == 'peek':
            samples = []
            for i in range(args.samples):
                if i:
                    await asyncio.sleep(args.interval)
                reads = []
                for start in args.addr:
                    chunks, addr, remaining = [], start, args.len
                    while remaining > 0:
                        body = await link.check(CFW_PEEK, struct.pack('<IB', addr, min(16, remaining)))
                        chunks.append(body)
                        addr += len(body)
                        remaining -= len(body)
                    data = b''.join(chunks)
                    reads.append({'addr': hex(start), 'len': len(data), 'hex': data.hex(),
                                  'nonzero': sum(1 for b in data if b), 'ascii': ''.join(chr(b) if 32 <= b < 127 else '.' for b in data)})
                sample = reads[0] if len(reads) == 1 else {'reads': reads}
                if args.samples > 1:
                    sample = {'t': round(i * args.interval, 2), **sample}
                    print(json.dumps(sample), file=sys.stderr, flush=True)
                samples.append(sample)
            result = samples[0] if args.samples == 1 else {'samples': samples}
        elif args.command == 'touch':
            def input_state(body):
                return {'notify_gestures': bool(body[0] & 1), 'swallow_gestures': bool(body[0] & 2),
                        'notify_events': bool(body[0] & 4), 'swallow_events': bool(body[0] & 8),
                        'last_gesture': TOUCH_CODES.get(body[1], body[1]), 'gestures': struct.unpack_from('<H', body, 2)[0],
                        'gestures_passed': struct.unpack_from('<H', body, 4)[0],
                        'auto_panel': not body[0] & 0x10,
                        'last_event': STOCK_EVENTS.get(body[6], body[6]), 'events': struct.unpack_from('<H', body, 7)[0],
                        'events_sent': struct.unpack_from('<H', body, 9)[0], 'panel_autos': body[11] if len(body) > 11 else None,
                        'work_mode': WORK_MODES.get(body[12], body[12]) if len(body) > 15 else None,
                        'last_mode_request': WORK_MODES.get(body[13], body[13]) if len(body) > 15 else None,
                        'mode_changes': body[14] if len(body) > 16 else None,
                        'checks': body[15] if len(body) > 16 else None,
                        'suspends_skipped': body[16] if len(body) > 16 else None}
            if args.action == 'state':
                result = input_state(await link.check(CFW_INPUT_STATE))
                # The wear word and its setting live in stock RAM; read them alongside.
                result.update(await link.wear_state())
            elif args.action == 'control':
                flags = ((0x01 if args.notify else 0) | (0x02 if args.swallow else 0)
                         | (0x04 if args.notify_events else 0) | (0x08 if args.swallow_events else 0)
                         | (0x10 if args.no_auto_panel else 0))
                payload = bytes([flags])
                if args.wear_on is not None or args.wear_off is not None:
                    if args.wear_on is None or args.wear_off is None or not 0 <= args.wear_off < args.wear_on <= 255:
                        raise FirmwareError('--wear-on and --wear-off go together, with on above off (0-255)')
                    payload += bytes([args.wear_on, args.wear_off])
                echoed = await link.check(CFW_INPUT_CONTROL, payload)
                result = input_state(await link.check(CFW_INPUT_STATE))
                if len(echoed) >= 3:
                    result['wear_on'], result['wear_off'] = echoed[1], echoed[2]
            else:
                flags = 0x01 | 0x04 | (0x02 if args.swallow else 0) | (0x08 if args.swallow_events else 0) | (0x10 if args.no_auto_panel else 0)
                await link.check(CFW_INPUT_CONTROL, bytes([flags]))
                events, started = [], asyncio.get_running_loop().time()
                deadline = started + args.seconds
                print(f'Listening for touches on {args.side} for {args.seconds:g} s', file=sys.stderr, flush=True)
                while True:
                    remaining = deadline - asyncio.get_running_loop().time()
                    if remaining <= 0:
                        break
                    try:
                        reply = await asyncio.wait_for(link.events.get(), remaining)
                    except asyncio.TimeoutError:
                        break
                    if len(reply) >= 7 and reply[1] == CFW_INPUT_EVENT:
                        kind, names = EVENT_CLASSES.get(reply[3], (f'class-{reply[3]}', {}))
                        event = {'t': round(asyncio.get_running_loop().time() - started, 2), 'kind': kind,
                                 'event': names.get(reply[4], reply[4]), 'count': struct.unpack_from('<H', reply, 5)[0]}
                        events.append(event)
                        print(json.dumps(event), file=sys.stderr, flush=True)
                if not args.keep:
                    await link.check(CFW_INPUT_CONTROL, bytes([0]))
                result = {'events': events, 'seconds': args.seconds, 'kept_flags': args.keep}
        elif args.command == 'view':
            def view_state(body):
                dx, dy, col, row = struct.unpack_from('<hhhh', body, 0)
                return {'dx': dx, 'dy': dy, 'stock_col': col, 'stock_row': row, 'effective_col': col + dx,
                        'effective_row': row + dy, 'height_gear': body[8], 'depth_gear': body[9]}
            current = view_state(await link.check(CFW_VIEW, bytes([1])))
            if args.height is None and args.depth is None and args.dx is None and args.dy is None:
                result = current
            else:
                dx, dy = current['dx'], current['dy']
                if args.dx is not None:
                    dx = args.dx
                if args.dy is not None:
                    dy = args.dy
                if args.height is not None:
                    dy = (8 - args.height) * 35 - current['stock_row']
                if args.depth is not None:
                    # The master (right) temple shifts the opposite way, as the stock UI does.
                    dx = DEPTH_SHIFT[args.depth] * (-1 if args.side == 'R' else 1)
                blanked = False
                if not args.no_flush and (dx, dy) != (current['dx'], current['dy']):
                    # The panel keeps what was pushed; blank the frame at the old position first,
                    # then move and push. The caller redraws its scene afterwards.
                    await link.check(CFW_FILL, bytes([0x02]) + rect_bytes(0, 0, FRAME_W, FRAME_H) + b'\x00')
                    blanked = True
                op = 0 | (0 if args.no_flush else 2)
                body = await link.check(CFW_VIEW, bytes([op]) + struct.pack('<hh', dx, dy))
                result = view_state(body)
                result.update({'flushed': not args.no_flush, 'blanked_old_position': blanked,
                               'note': 'frame cleared; redraw your scene' if blanked else None})
        elif args.command == 'mic':
            def mic_state(body):
                return {'recording': bool(body[0]), 'starts': struct.unpack_from('<H', body, 1)[0],
                        'stops': struct.unpack_from('<H', body, 3)[0],
                        'mode': {0: 'stream', 1: 'stopped', 2: 'capture-only'}.get(body[5], body[5]), 'session_armed': bool(body[6])}
            if args.action == 'state':
                result = mic_state(await link.check(CFW_MIC_CONTROL, bytes([2])))
            elif args.action == 'stop':
                result = mic_state(await link.check(CFW_MIC_CONTROL, bytes([0])))
            else:
                result = mic_state(await link.check(CFW_MIC_CONTROL, bytes([1])))
                chunks, seqs, started = [], [], asyncio.get_running_loop().time()
                deadline = started + args.seconds
                try:
                    while True:
                        remaining = deadline - asyncio.get_running_loop().time()
                        if remaining <= 0:
                            break
                        try:
                            packet = await asyncio.wait_for(link.audio.get(), remaining)
                        except asyncio.TimeoutError:
                            break
                        seqs.append(packet[1])
                        chunks.append(packet[2:])
                        if len(chunks) == 1:
                            print(f'first audio packet after {asyncio.get_running_loop().time() - started:.2f} s, {len(packet)} bytes', file=sys.stderr, flush=True)
                finally:
                    result.update({'stopped': mic_state(await link.check(CFW_MIC_CONTROL, bytes([0])))})
                elapsed = asyncio.get_running_loop().time() - started
                lost = sum(1 for a, b in zip(seqs, seqs[1:]) if (b - a) & 0xFF != 1)
                payload = b''.join(chunks)
                if args.out:
                    args.out.write_bytes(payload)
                result.update({'seconds': round(elapsed, 2), 'packets': len(chunks), 'bytes': len(payload),
                               'chunk_bytes': sorted({len(c) for c in chunks}), 'sequence_gaps': lost,
                               'packets_per_second': round(len(chunks) / elapsed, 1) if elapsed else None,
                               'out': str(args.out) if args.out else None})
        elif args.command == 'imu' and args.stream:
            period = args.period_ms
            body = await link.check(CFW_IMU_CONTROL, bytes([period]) + struct.pack('<H', args.samples))
            samples, started = [], asyncio.get_running_loop().time()
            deadline = started + args.samples * period / 1000 + 5
            while len(samples) < args.samples:
                remaining = deadline - asyncio.get_running_loop().time()
                if remaining <= 0:
                    break
                try:
                    reply = await asyncio.wait_for(link.events.get(), remaining)
                except asyncio.TimeoutError:
                    break
                if len(reply) >= 17 and reply[1] == CFW_IMU_SAMPLE:
                    seq, a, b, pitch = struct.unpack_from('<Hiii', reply, 3)
                    sample = {'t': round(asyncio.get_running_loop().time() - started, 3), 'seq': seq,
                              'word0': a / 100, 'word1': b / 100, 'pitch': pitch / 100}
                    samples.append(sample)
                    print(json.dumps(sample), file=sys.stderr, flush=True)
            await link.check(CFW_IMU_CONTROL, bytes([period, 0, 0]))
            span = (samples[-1]['t'] - samples[0]['t']) if len(samples) > 1 else 0
            result = {'period_ms': period, 'requested': args.samples, 'received': len(samples), 'seconds': round(span, 2),
                      'rate_hz': round((len(samples) - 1) / span, 1) if span else None, 'samples': samples}
        elif args.command == 'imu':
            # Attitude words kept by the IMU fusion thread on the master (right) temple,
            # in hundredths of a degree; the third is the pitch the head-up detector uses.
            imu_addr = (await link.stock_layout())['imu']
            samples = []
            for i in range(args.samples):
                if i:
                    await asyncio.sleep(args.interval)
                body = await link.check(CFW_PEEK, struct.pack('<IB', imu_addr, 12))
                a, b, pitch = struct.unpack('<iii', body)
                sample = {'t': round(i * args.interval, 2), 'word0': a / 100, 'word1': b / 100, 'pitch': pitch / 100}
                samples.append(sample)
                if args.samples > 1:
                    print(json.dumps(sample), file=sys.stderr, flush=True)
            result = {'address': hex(imu_addr), 'samples': samples}
        elif args.command == 'stats':
            body = await link.check(CFW_STATS)
            result = {'messages': struct.unpack_from('<H', body, 0)[0], 'errors': struct.unpack_from('<H', body, 2)[0],
                      'stock_swallowed': struct.unpack_from('<H', body, 4)[0], 'last_status': CFW_STATUS.get(body[6], body[6]),
                      'message_active': bool(body[7]), 'message_bytes': struct.unpack_from('<H', body, 8)[0],
                      'message_max': body[10] << 8, 'ram_bytes': body[11] << 8}
        elif args.command == 'draw':
            if args.pbm:
                width, height, rows = read_pbm(args.pbm)
            else:
                width, height, rows = FRAME_W, FRAME_H, test_pattern_rows()
            started = asyncio.get_running_loop().time()
            flush_rect = rect_bytes(0, 0, FRAME_W, FRAME_H) if args.clear else rect_bytes(args.x, args.y, width, height)
            if args.stream:
                sender = StreamSender(link)
                stride = (width + 7) // 8
                flags = 0x01 | (0x04 if args.rle else 0) | ((args.level & 0xF) << 4)
                records = []
                if args.clear:
                    records.append(record(CFW_FILL, bytes([0x00]) + rect_bytes(0, 0, FRAME_W, FRAME_H) + b'\x00'))
                band = max(1, (CFW_MSG_MAX - 64) // stride)
                for start in range(0, height, band):
                    chunk = rows[start:start + band]
                    pixels = b''.join(chunk)
                    if args.rle:
                        pixels = packbits(pixels)
                    records.append(record(CFW_BLIT, bytes([flags]) + rect_bytes(args.x, args.y + start, width, len(chunk)) + pixels))
                if not args.no_flush:
                    records.append(record(CFW_FLUSH, flush_rect))
                messages, current, raw_bytes = [], b'', 0
                for rec in records:
                    raw_bytes += len(rec)
                    if len(current) + len(rec) > CFW_MSG_MAX:
                        messages.append(current)
                        current = b''
                    if len(rec) > CFW_MSG_MAX:
                        raise FirmwareError('record larger than one message; use a smaller band')
                    current += rec
                messages.append(current)
                done = sum([await sender.send(m) for m in messages])
                result = {'drawn': [args.x, args.y, width, height], 'messages': len(messages), 'records': done,
                          'packets': sender.packets, 'payload_bytes': raw_bytes, 'rle': args.rle}
            else:
                if args.clear:
                    await link.check(CFW_FILL, bytes([0x00]) + rect_bytes(0, 0, FRAME_W, FRAME_H) + b'\x00')
                sent = 0
                for payload in blit_packets(args.x, args.y, width, rows, args.level, flush=False):
                    await link.check(CFW_BLIT, payload)
                    sent += 1
                if not args.no_flush:
                    await link.check(CFW_FLUSH, flush_rect)
                result = {'drawn': [args.x, args.y, width, height], 'packets': sent}
            deferred = args.rle and sender.deferred
            result.update({'source': str(args.pbm or 'test-pattern'), 'flushed': not args.no_flush and not deferred,
                           'seconds': round(asyncio.get_running_loop().time() - started, 2)})
            if deferred:
                result['deferred'] = 'panel off: the frame shows when it comes on'
        else:
            raise FirmwareError(args.command)
    checkpoint(args.command, result)
    emit(result)


def prepare(args):
    """Validate the complete image before importing Bluetooth or opening a device."""
    if not getattr(args, 'firmware', None):
        return None, None
    data = args.firmware.read_bytes()
    return data, plan_image(data, load_profiles(), PUBLIC_KEY.read_bytes())


async def run(args, scanner_cls=None, client_cls=None, checkpoint=lambda *_: None):
    image, plan = prepare(args)
    if args.command == 'plan':
        emit(plan.report())
        return
    real_bluetooth = scanner_cls is None or client_cls is None
    if real_bluetooth:
        from bleak import BleakClient, BleakScanner
        scanner_cls, client_cls = BleakScanner, BleakClient
    if args.command == 'scan':
        found = await scanner_cls.discover(timeout=args.scan_seconds, return_adv=True)
        result = [{'address': device.address, 'name': ad.local_name or device.name,
                   'rssi': ad.rssi, 'services': ad.service_uuids}
                  for device, ad in found.values() if is_g1(ad.local_name or device.name)]
        checkpoint('scan-result', result)
        emit(result)
        return

    names = {}

    def matches(device, advertisement):
        name = advertisement.local_name or device.name
        if device.address.casefold() == args.device.casefold() and is_g1(name, args.side):
            names[device.address] = name
            return True
        return False

    device = await scanner_cls.find_device_by_filter(matches, timeout=args.scan_seconds)
    if device is None:
        raise FirmwareError('Explicit device was not found as a G1 of the selected side')
    checkpoint('device-selected', {'address': device.address, 'name': names[device.address]})
    session = {}

    def on_disconnect(_client):
        # Stock firmware drops an unbonded central about 30 s after connecting.
        # Fail the pending SMP request now instead of waiting out its timeout.
        checkpoint('disconnected', {'address': device.address})
        if 'smp' in session:
            session['smp'].responses.put_nowait(FirmwareError('Device disconnected'))

    options = {'disconnected_callback': on_disconnect} if real_bluetooth else {}
    async with client_cls(device, timeout=20, **options) as client:
        if args.command in ('info', 'fill', 'draw', 'panel', 'stats', 'text', 'asset', 'shapes', 'peek', 'obj', 'touch', 'imu', 'view', 'mic'):
            await run_cfw_command(client, args, checkpoint)
            return
        if args.command == 'ping':
            result = await query_ping(client)
            if args.hold:
                # Prove the link survives longer than the stock 30 s unbonded drop.
                await asyncio.sleep(args.hold)
                result = {'first': result, 'held_seconds': args.hold, 'after_hold': await query_ping(client)}
            checkpoint('ping', result)
            emit(result)
            return
        if args.command in ('probe', 'verify', 'enter-dfu'):
            reply = await query_marker(client)
            result = {'address': device.address, 'name': names[device.address], 'reply_hex': reply.hex(),
                      'reply': reply.rstrip(b'\0').decode('utf-8', errors='replace'), **diagnostic(reply)}
            checkpoint('diagnostic', result)
            emit(result)
            if args.command == 'verify':
                verify_reply(reply, args.expect)
                checkpoint('diagnostic-verified', {'expected': args.expect})
            if args.command == 'enter-dfu':
                # Only the base we have inspected or this exact CFW may be rebooted here.
                verify_reply(reply, 'cfw' if result['cfw_marker'] else 'stock')
                characteristic = client.services.get_characteristic(NUS_RX)
                checkpoint('dfu-entry-requested', {'command': '4a01', 'scope': 'local-temple'})
                await client.write_gatt_char(characteristic, b'\x4a\x01',
                                            response='write' in characteristic.properties)
                emit({'status': 'local DFU request sent; rescan and run state to verify transition'})
            return

        smp = SMP(client)
        session['smp'] = smp
        await smp.start()
        if args.command == 'state':
            state = await smp.state()
            checkpoint('image-state', state)
            emit(state)
            return
        checkpoint('flash-plan', plan.report())
        if args.command == 'stage':
            last = [-1]

            def progress(current, total):
                percent = current * 100 // total
                if percent != last[0]:
                    print(f'Upload {percent}% ({current}/{total})', file=sys.stderr, flush=True)
                    last[0] = percent

            state = await upload(smp, image, plan.source_hashes, plan.target_hash, progress,
                                 chunk=args.chunk_bytes)
            result = {'status': 'uploaded; inactive hash verified; not activated', 'state': state}
        elif args.command == 'confirm':
            result = await confirm(smp, plan.target_hash)
        else:
            result = await activate(smp, plan.source_hashes, plan.target_hash,
                                    permanent=args.permanent, checkpoint=checkpoint)
        checkpoint('result', result)
        emit(result)


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--_worker', action='store_true', help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest='command', required=True)
    offline = sub.add_parser('plan', help='Verify an image and print its flash/restore plan without Bluetooth')
    offline.add_argument('--firmware', type=Path, required=True)
    for command in ('scan', 'probe', 'ping', 'info', 'stats', 'panel', 'fill', 'draw', 'text', 'asset', 'shapes', 'peek', 'obj', 'touch', 'imu', 'view', 'mic', 'verify', 'enter-dfu', 'state', 'stage', 'activate', 'confirm'):
        child = sub.add_parser(command)
        child.add_argument('--scan-seconds', type=float, default=10)
        child.add_argument('--log', type=Path, help='New JSONL session log (must not already exist)')
        if command != 'scan':
            child.add_argument('--device', required=True, help='Explicit G1 address/UUID from scan')
            child.add_argument('--side', choices=('L', 'R'), required=True)
        if command in ('stage', 'activate', 'confirm'):
            child.add_argument('--firmware', type=Path, required=True,
                               help='Pinned CFW to install, or pinned stock application to restore')
        if command == 'stage':
            child.add_argument('--chunk-bytes', type=int, default=CHUNK_BYTES,
                               help='Maximum image bytes per SMP upload request (1-480)')
        if command == 'activate':
            child.add_argument('--permanent', action='store_true',
                               help='Explicit permanent selection instead of a test boot; rollback is not promised')
        if command == 'verify':
            child.add_argument('--expect', choices=('cfw', 'stock'), default='cfw')
        if command == 'ping':
            child.add_argument('--hold', type=float, default=0, help='Stay connected this many seconds, then ping again')
        if command == 'panel':
            group = child.add_mutually_exclusive_group(required=True)
            group.add_argument('--on', action='store_true', help='Resume the panel via the stock system-resume path')
            group.add_argument('--off', action='store_true', help='Suspend the panel via the stock system-suspend path')
            group.add_argument('--force-on', action='store_true', help="Call the panel context's own on callback (ignores dock/wear policy)")
            group.add_argument('--force-off', action='store_true', help="Call the panel context's own off callback")
            group.add_argument('--resume', action='store_true', help="Call the panel context's resume callback (panel init + brightness; what the auto panel uses)")
            group.add_argument('--suspend', action='store_true', help="Call the panel context's suspend callback (SPI deep power down)")
            group.add_argument('--wake', action='store_true', help='Driver resume only (panel init + brightness, rail untouched) and set the powered flag: what the auto panel does')
            group.add_argument('--sleep', action='store_true', help='Driver suspend only (SPI deep power down, rail untouched) and clear the powered flag')
        if command == 'fill':
            child.add_argument('--rect', type=lambda v: tuple(int(x) for x in v.split(',')), default=(0, 0, 640, 200),
                               help='x,y,w,h (even x and w)')
            child.add_argument('--level', type=int, default=0, help='grey level 0-15')
        if command == 'draw':
            child.add_argument('--pbm', type=Path, help='Binary PBM (P4) up to 640x200; default is a test pattern')
            child.add_argument('--x', type=int, default=0)
            child.add_argument('--y', type=int, default=0)
            child.add_argument('--level', type=int, default=15, help='grey level 1-15 for set pixels')
            child.add_argument('--clear', action='store_true', help='Clear the frame first and flush the whole panel')
        if command == 'text':
            child.add_argument('text', help='UTF-8 text (ASCII glyphs come from internal flash)')
            child.add_argument('--font', type=int, default=0, choices=(0, 1, 2), help='0 = 20 px proportional, 1/2 = digit faces')
            child.add_argument('--box', type=lambda v: tuple(int(x) for x in v.split(',')), default=(32, 32, 608, 59),
                               help='x0,y0,x1,y1 (even x; y1 must leave room for a glyph cell)')
            child.add_argument('--lines', type=int, default=1)
            child.add_argument('--clear', action='store_true', help='Clear the box before drawing')
        if command == 'asset':
            child.add_argument('action', choices=('put', 'draw', 'text', 'clear', 'info'))
            child.add_argument('--id', type=int, default=1)
            child.add_argument('--pbm', type=Path, help='put: binary PBM image (default: test pattern)')
            child.add_argument('--string', help='put: store a UTF-8 string asset instead of an image')
            child.add_argument('--at', type=lambda v: tuple(int(x) for x in v.split(',')), default=(0, 0), help='draw: x,y')
            child.add_argument('--level', type=int, default=15, help='draw: grey level for 1bpp assets')
            child.add_argument('--font', type=int, default=0, choices=(0, 1, 2))
            child.add_argument('--box', type=lambda v: tuple(int(x) for x in v.split(',')), default=(32, 32, 608, 59))
            child.add_argument('--lines', type=int, default=1)
            child.add_argument('--clear', action='store_true', help='text: clear the box first')
        if command == 'obj':
            child.add_argument('action', choices=('put', 'show', 'hide', 'state', 'drop', 'anim'))
            child.add_argument('--move', action='append', default=[], help='anim: id:tx,ty:frames[:easing] (0 linear, 1 smooth, 2 out, 3 in)')
            child.add_argument('--tween', action='append', default=[],
                               help='anim: id:mask:frames:easing:v0,v1,... (mask bits 0-7 params, 0x100 colour, 0x200 width)')
            child.add_argument('--record', default=None, help='put --kind shape: type,color,width,p0,... (a SHAPES record)')
            child.add_argument('--period', type=int, default=20, help='anim: ms per frame (1-250; 0 keeps current). 20 gives about 30 fps')
            child.add_argument('--wait', action='store_true', help='anim: wait for the glides to finish and report frames per second')
            child.add_argument('--id', type=int, default=None)
            child.add_argument('--version', type=int, default=1)
            child.add_argument('--kind', choices=('image', 'text', 'rect', 'shapes', 'shape'), default='rect')
            child.add_argument('--at', type=lambda v: tuple(int(x) for x in v.split(',')), default=(0, 0))
            child.add_argument('--size', type=lambda v: tuple(int(x) for x in v.split(',')), default=(0, 0), help='w,h for rect/text boxes')
            child.add_argument('--asset', type=int, default=0)
            child.add_argument('--level', type=int, default=15)
            child.add_argument('--font', type=int, default=0)
            child.add_argument('--lines', type=int, default=1)
            child.add_argument('--hidden', action='store_true')
            child.add_argument('--ids', help='show: comma-separated object ids in draw order')
            child.add_argument('--clear', action='store_true', help='show: clear the frame first')
        if command == 'touch':
            child.add_argument('action', choices=('watch', 'state', 'control'))
            child.add_argument('--seconds', type=float, default=20, help='watch: how long to report gestures')
            child.add_argument('--swallow', action='store_true', help='keep gestures from the stock handler while set')
            child.add_argument('--notify', action='store_true', help='control: send INPUT_EVENT for each gesture')
            child.add_argument('--notify-events', action='store_true', help='control: send INPUT_EVENT for each stock event (head, wear, case)')
            child.add_argument('--swallow-events', action='store_true', help='keep stock events from the phone path while set')
            child.add_argument('--no-auto-panel', action='store_true', help='leave panel power to the host instead of following wear events')
            child.add_argument('--keep', action='store_true', help='watch: leave notify/swallow set after returning')
            child.add_argument('--wear-on', type=int, help='control: wear word at or above this means worn (default 56)')
            child.add_argument('--wear-off', type=int, help='control: wear word at or below this means not worn (default 53)')
        if command == 'imu':
            child.add_argument('--samples', type=int, default=1, help='attitude samples to read (right temple holds the IMU)')
            child.add_argument('--interval', type=float, default=0.5, help='seconds between samples')
            child.add_argument('--stream', action='store_true', help='have the glasses push samples (G1CFW/23+) instead of polling PEEK')
            child.add_argument('--period-ms', type=int, default=50, help='stream: ms between samples (10-250)')
        if command == 'mic':
            child.add_argument('action', choices=('record', 'stop', 'state'))
            child.add_argument('--seconds', type=float, default=5, help='record: how long to capture the stock LC3 stream (1-60)')
            child.add_argument('--out', type=Path, help='record: write the concatenated LC3 chunks here')
        if command == 'view':
            child.add_argument('--height', type=int, choices=range(0, 9), help='stock height gear 0-8 (row origin (8-gear)*35)')
            child.add_argument('--depth', type=int, choices=range(1, 10), help='stock depth gear 1-9 (opposite column shift per temple)')
            child.add_argument('--dx', type=int, help='column offset added to the stock origin')
            child.add_argument('--dy', type=int, help='row offset added to the stock origin')
            child.add_argument('--no-flush', action='store_true', help='set the offsets without pushing the frame')
        if command == 'peek':
            child.add_argument('--addr', type=lambda v: int(v, 0), action='append', required=True,
                               help='start address; repeat to read several ranges per sample')
            child.add_argument('--len', type=int, default=64, help='bytes to read (16 per request)')
            child.add_argument('--samples', type=int, default=1, help='read the range this many times over one connection')
            child.add_argument('--interval', type=float, default=0.5, help='seconds between samples')
        if command == 'shapes':
            child.add_argument('--demo', action='store_true', help='Draw a built-in shapes demo')
            child.add_argument('--shape', action='append', default=[],
                               help='type,color,width,p0,...  types: 1 line x0,y0,x1,y1; 2 rect x,y,w,h,r; 3 rect fill; 4 circle cx,cy,r; '
                                    '5 disc; 6 triangle; 7 triangle fill; 8 quad; 9 quad fill; 10 quadratic; 11 cubic; 12 arc cx,cy,r,a0,a1; 13 pie')
        if command in ('fill', 'draw', 'text', 'asset', 'shapes', 'obj'):
            child.add_argument('--no-flush', action='store_true', help='Write the frame buffer only; do not push to the panel')
        if command == 'draw':
            child.add_argument('--stream', action='store_true', help='Send as pipelined multi-packet messages (G1CFW/9+)')
            child.add_argument('--rle', action='store_true', help='PackBits-compress pixel rows (implies --stream)')
    args = parser.parse_args(argv)
    if getattr(args, 'scan_seconds', 10) <= 0 or getattr(args, 'scan_seconds', 10) > 30:
        parser.error('--scan-seconds must be greater than 0 and at most 30')
    if not 1 <= getattr(args, 'chunk_bytes', CHUNK_BYTES) <= 480:
        parser.error('--chunk-bytes must be between 1 and 480')
    if getattr(args, 'rle', False):
        args.stream = True
    if getattr(args, 'command', None) == 'mic' and args.action == 'record' and not 1 <= args.seconds <= 60:
        parser.error('--seconds must be between 1 and 60')
    if getattr(args, 'command', None) == 'imu':
        if args.stream and not (10 <= args.period_ms <= 250 and 1 <= args.samples <= 65534 and args.samples * args.period_ms <= 75000):
            parser.error('imu --stream needs a 10-250 ms period and at most 75 s of samples')
        if not args.stream and not 1 <= args.samples * max(args.interval, 0.2) <= 75:
            parser.error('imu sampling must take between 1 and 75 s')
    if getattr(args, 'command', None) == 'touch' and not 1 <= args.seconds <= 75:
        parser.error('--seconds must be between 1 and 75 (the Bluetooth worker deadline is 90 s)')
    return parser, args


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    parser, args = arguments(argv)
    journal = None
    try:
        prepare(args)
        if args.command != 'plan' and not args._worker:
            # A native CoreBluetooth permission prompt can block the Python event
            # loop. Enforce the deadline in a parent process, not just asyncio.
            limit = 1800 if args.command == 'stage' else 90
            if args.command == 'peek' and args.samples > 1:
                # Each sample is a round trip per address range on top of the pause.
                limit = max(limit, 60 + args.samples * (args.interval + 0.5 * len(args.addr)))
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--_worker', *argv],
                                    timeout=limit, check=False)
            return result.returncode
        if args.command != 'plan':
            log_path = args.log or ROOT / 'build/logs' / f'{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{uuid.uuid4().hex[:8]}.jsonl'
            journal = Journal(log_path)
            journal.event('start', {'command': args.command, 'device': getattr(args, 'device', None),
                                    'side': getattr(args, 'side', None)})
            print(f'Session log: {log_path}', file=sys.stderr, flush=True)
        asyncio.run(run(args, checkpoint=journal.event if journal else lambda *_: None))
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        if journal:
            journal.event('error', {'type': type(exc).__name__, 'message': str(exc),
                                    'action': 'Inspect last recorded step and device state before retrying; boot is unverified. '
                                              'If `state` answers but UART commands do not, the glasses are in deep sleep '
                                              '(case closed, no charger: stock work mode 8 pauses the BLE command worker); '
                                              'open the case lid or dock them.'})
        if isinstance(exc, subprocess.TimeoutExpired):
            message = 'Bluetooth worker exceeded its deadline and was stopped. Check permission/connection and the session log before retrying.'
        else:
            message = str(exc) or 'Operation interrupted or timed out'
        print(f'{type(exc).__name__}: {message}', file=sys.stderr)
        return 1
    finally:
        if journal:
            journal.close()


if __name__ == '__main__':
    sys.exit(main())
