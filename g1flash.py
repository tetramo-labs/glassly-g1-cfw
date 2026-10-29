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
    if result['build'] != '2025-03-21 11:38:43' or result['version'] != '1.5.60':
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
CFW_ASSET_PUT, CFW_ASSET_DRAW, CFW_ASSET_TEXT, CFW_ASSET_CLEAR, CFW_ASSET_INFO, CFW_STREAM, CFW_STATS = 8, 9, 10, 11, 12, 0x10, 0x11
CFW_STATUS = {0: 'ok', 1: 'bad-length', 2: 'bad-command', 3: 'bad-rect', 4: 'panel-off', 5: 'bad-seq',
              6: 'bad-crc', 7: 'too-big', 8: 'bad-record', 9: 'bad-rle', 10: 'bad-font', 11: 'no-memory',
              12: 'no-asset', 13: 'bad-asset'}
CFW_MSG_MAX = 4096
FRAME_W, FRAME_H, MAX_PACKET = 640, 200, 255


class CfwLink:
    """Request/reply helper for the private 0xF0 protocol over the Nordic UART service."""

    def __init__(self, client):
        self.client = client
        self.rx = client.services.get_characteristic(NUS_RX)
        self.replies = asyncio.Queue()

    async def __aenter__(self):
        if not self.rx or not self.client.services.get_characteristic(NUS_TX):
            raise FirmwareError('G1 Nordic UART service absent')
        await self.client.start_notify(NUS_TX, lambda _s, d: self.replies.put_nowait(bytes(d)) if d[:1] == b'\xf0' else None)
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


class StreamSender:
    """Pipelined multi-packet messages: packets go out without waiting; only
    the last packet of a message is answered."""

    def __init__(self, link):
        self.link = link
        self.seq = 0
        self.packets = 0

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
        for i, (is_first, data) in enumerate(chunks):
            flags = (0x01 if is_first else 0) | (0x02 if i == len(chunks) - 1 else 0)
            packet = bytes([CFW_SID, CFW_STREAM, self.seq, flags]) + data
            await self.link.client.write_gatt_char(self.link.rx, packet, response=False)
            self.seq = (self.seq + 1) & 0xFF
            self.packets += 1
        while True:
            reply = await asyncio.wait_for(self.link.replies.get(), timeout)
            if len(reply) >= 5 and reply[1] == CFW_STREAM:
                status, seq, done = reply[2], reply[3], reply[4]
                if status != 0:
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
            mode = {'on': 1, 'off': 0, 'force_on': 2, 'force_off': 3}
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
        elif args.command == 'copy':
            x, y, w, h = args.rect
            flags = 0 if args.no_flush else 0x02
            await link.check(CFW_COPY, bytes([flags]) + rect_bytes(x, y, w, h) + struct.pack('<HH', args.to[0], args.to[1]))
            result = {'copied': args.rect, 'to': args.to, 'flushed': not args.no_flush}
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
            result.update({'source': str(args.pbm or 'test-pattern'), 'flushed': not args.no_flush,
                           'seconds': round(asyncio.get_running_loop().time() - started, 2)})
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
        if args.command in ('info', 'fill', 'draw', 'panel', 'stats', 'text', 'copy', 'asset'):
            await run_cfw_command(client, args, checkpoint)
            return
        if args.command == 'ping':
            result = await query_ping(client)
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
    for command in ('scan', 'probe', 'ping', 'info', 'stats', 'panel', 'fill', 'draw', 'text', 'copy', 'asset', 'verify', 'enter-dfu', 'state', 'stage', 'activate', 'confirm'):
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
        if command == 'panel':
            group = child.add_mutually_exclusive_group(required=True)
            group.add_argument('--on', action='store_true', help='Resume the panel via the stock system-resume path')
            group.add_argument('--off', action='store_true', help='Suspend the panel via the stock system-suspend path')
            group.add_argument('--force-on', action='store_true', help="Call the panel context's own on callback (ignores dock/wear policy)")
            group.add_argument('--force-off', action='store_true', help="Call the panel context's own off callback")
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
        if command == 'copy':
            child.add_argument('--rect', type=lambda v: tuple(int(x) for x in v.split(',')), required=True, help='source x,y,w,h')
            child.add_argument('--to', type=lambda v: tuple(int(x) for x in v.split(',')), required=True, help='destination x,y')
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
        if command in ('fill', 'draw', 'text', 'copy', 'asset'):
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
                                    'action': 'Inspect last recorded step and device state before retrying; boot is unverified'})
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
