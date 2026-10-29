"""Conservative stop-and-wait MCUmgr SMP v1 over BLE.

Each request fits one ATT write. No implicit retry of timed-out writes and no
network-core upload, erase command, or bootloader operation.
"""
import asyncio
import hashlib
import io
import struct

import cbor2

from .image import FirmwareError

SMP_UUID = 'da2e7828-fbce-4e01-ae9e-261174997c48'
NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'
NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'


def encode(op, group, command, sequence, payload):
    body = cbor2.dumps(payload)
    return struct.pack('>BBHHBB', op, 0, len(body), group, sequence, command) + body


class Frames:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        frames = []
        while len(self.buffer) >= 8:
            op, flags, length, group, sequence, command = struct.unpack_from('>BBHHBB', self.buffer)
            if length > 8192 or op not in (1, 3) or flags:
                raise FirmwareError('Invalid SMP response header')
            if len(self.buffer) < 8 + length:
                break
            stream = io.BytesIO(self.buffer[8:8 + length])
            try:
                payload = cbor2.load(stream)
            except (ValueError, EOFError) as exc:
                raise FirmwareError('Malformed SMP CBOR') from exc
            if stream.read(1) or not isinstance(payload, dict):
                raise FirmwareError('SMP response must contain exactly one CBOR map')
            del self.buffer[:8 + length]
            frames.append((op, group, command, sequence, payload))
        return frames


class SMP:
    def __init__(self, client, timeout=20):
        self.client = client
        self.timeout = timeout
        self.frames = Frames()
        self.responses = asyncio.Queue()
        self.sequence = 0
        self.characteristic = client.services.get_characteristic(SMP_UUID)
        if self.characteristic is None:
            raise FirmwareError('MCUmgr SMP characteristic absent; firmware is not in DFU mode')
        # Do not rely on multi-write SMP reassembly being configured in stock.

    @property
    def limit(self):
        # Some backends initially report 20 and update after MTU negotiation.
        return self.characteristic.max_write_without_response_size

    def notify(self, _sender, data):
        try:
            for frame in self.frames.feed(data):
                self.responses.put_nowait(frame)
        except Exception as exc:
            self.responses.put_nowait(exc)

    async def start(self):
        await self.client.start_notify(self.characteristic, self.notify)

    async def request(self, op, group, command, payload):
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 255
        packet = encode(op, group, command, sequence, payload)
        if len(packet) > self.limit:
            raise FirmwareError(f'SMP request needs {len(packet)} ATT bytes; negotiated limit is {self.limit}')
        async with asyncio.timeout(self.timeout):
            await self.client.write_gatt_char(self.characteristic, packet, response=False)
            while True:
                response = await self.responses.get()
                if isinstance(response, Exception):
                    raise response
                rop, rgroup, rcommand, rsequence, result = response
                if rsequence != sequence:
                    continue  # Delayed response to an earlier request.
                if (rop, rgroup, rcommand) != (op + 1, group, command):
                    raise FirmwareError('Mismatched SMP response operation/group/command')
                if result.get('rc', 0) != 0 or result.get('err'):
                    raise FirmwareError(f'Device rejected SMP request: {result}')
                return result

    async def state(self):
        return await self.request(0, 1, 0, {})


def image_rows(state):
    rows = state.get('images')
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise FirmwareError('Malformed image-state response')
    return rows


def require_current(state, expected, allowed_pending=None):
    rows = image_rows(state)
    # Resume activation only for the identical pending application; never another image/core.
    if any(row.get('pending') and not (
        allowed_pending is not None and row.get('hash') == allowed_pending
        and row.get('image', 0) == 0 and row.get('slot') == 1
        and not row.get('active')) for row in rows):
        raise FirmwareError('An image is already pending; resolve its state before flashing')
    accepted = (expected,) if isinstance(expected, (bytes, bytearray)) else tuple(expected)
    active = [row for row in rows if row.get('image', 0) == 0 and row.get('active')]
    if len(active) != 1 or active[0].get('hash') not in accepted or active[0].get('slot') != 0:
        raise FirmwareError('Running application hash is not an accepted base for this image; refusing downgrade/rebase')
    return active[0]


def candidate(state, digest):
    rows = [row for row in image_rows(state)
            if row.get('image', 0) == 0 and row.get('hash') == digest
            and row.get('slot') == 1 and not row.get('active') and row.get('bootable') is True]
    if len(rows) != 1:
        raise FirmwareError('Device did not report the exact uploaded hash as a bootable inactive application')
    return rows[0]


CHUNK_BYTES = 128


def upload_packet(data, offset, limit, chunk=CHUNK_BYTES):
    payload = {'off': offset, 'data': b''}
    if offset == 0:
        payload.update(image=0, len=len(data), sha=hashlib.sha256(data).digest())
    # Bounded small ATT writes also fit the stock SMP buffer at large MTUs.
    for count in range(min(chunk, len(data) - offset), 0, -1):
        payload['data'] = data[offset:offset + count]
        if len(encode(2, 1, 1, 0, payload)) <= limit:
            return payload
    raise FirmwareError('ATT MTU too small for an SMP upload; negotiate a larger MTU before writing')


async def upload(smp, data, expected_current, expected_output, progress=lambda *_: None,
                 chunk=CHUNK_BYTES):
    current = require_current(await smp.state(), expected_current)
    if current.get('confirmed') is not True:
        raise FirmwareError('Confirm the current image before overwriting its recovery slot')
    upload_packet(data, 0, smp.limit, chunk)  # Fail before the first flash write for a small MTU.
    offset = 0
    while offset < len(data):
        payload = upload_packet(data, offset, smp.limit, chunk)
        reply = await smp.request(2, 1, 1, payload)
        next_offset = reply.get('off')
        if type(next_offset) is not int or not offset < next_offset <= len(data):
            raise FirmwareError(f'Invalid/non-progressing upload offset: {next_offset!r}')
        # The first reply can resume a previous upload by full-file SHA256.
        if offset != 0 and next_offset != offset + len(payload['data']):
            raise FirmwareError('Unexpected upload acknowledgement; restart to resume by image hash')
        offset = next_offset
        progress(offset, len(data))
    state = await smp.state()
    require_current(state, expected_current)
    candidate(state, expected_output)
    return state


async def activate(smp, expected_current, expected_output, permanent=False, checkpoint=lambda *_: None):
    state = await smp.state()
    if any(row.get('image', 0) == 0 and row.get('active') and row.get('hash') == expected_output
           for row in image_rows(state)):
        row = require_current(state, expected_output)
        return {'status': 'target-already-active; no reset sent', 'confirmed': row.get('confirmed') is True}
    require_current(state, expected_current, allowed_pending=expected_output)
    selected = candidate(state, expected_output)
    if selected.get('pending'):
        if bool(selected.get('permanent')) != permanent:
            raise FirmwareError('Pending image has a different permanence setting; no reset sent')
    else:
        checkpoint('select-image', {'hash': expected_output, 'permanent': permanent})
        state = await smp.request(2, 1, 0, {'hash': expected_output, 'confirm': permanent})
        require_current(state, expected_current, allowed_pending=expected_output)
        selected = candidate(state, expected_output)
        if not selected.get('pending') or bool(selected.get('permanent')) != permanent:
            raise FirmwareError('Device did not report the requested pending state; no reset sent')
    checkpoint('reset-requested', {'hash': expected_output})
    # A timeout/disconnect may occur after the reset was delivered. Callers must
    # record an uncertain outcome and reconnect, never blindly replay the write.
    await smp.request(2, 0, 5, {})
    return {'status': 'reset-acknowledged; boot verification required', 'boot_verified': False}


async def confirm(smp, expected):
    state = await smp.state()
    current = require_current(state, expected)
    if current.get('confirmed') is True:
        return {'status': 'already-confirmed', 'state': state}
    state = await smp.request(2, 1, 0, {'hash': expected, 'confirm': True})
    current = require_current(state, expected)
    if current.get('confirmed') is not True:
        raise FirmwareError('Device did not confirm the expected active image')
    return {'status': 'active-image-confirmed', 'state': state}
