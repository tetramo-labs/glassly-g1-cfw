"""Offline GATT/MCUboot simulator: exercises production framing and operations.

This models protocol behavior, not the production G1 bootloader or peripherals.
"""
import asyncio
from contextlib import redirect_stdout, redirect_stderr
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import cbor2
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

from cfw.image import FirmwareError, inspect_image, sha256
from cfw.plan import plan_image
from cfw.smp import SMP, SMP_UUID, NUS_RX, NUS_TX, activate, confirm, upload
import g1flash


def signed_image(key, body):
    header = struct.pack('<IIHHIIBBHII', 0x96F3B83D, 0, 512, 0, len(body), 0, 0, 0, 0, 0, 0)
    region = header.ljust(512, b'\0') + body
    public = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.PKCS1)
    signature = key.sign(region, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
    entries = [(0x10, hashlib.sha256(region).digest()), (1, hashlib.sha256(public).digest()), (0x20, signature)]
    tlv = b''.join(struct.pack('<BBH', kind, 0, len(value)) + value for kind, value in entries)
    return region + struct.pack('<HH', 0x6907, len(tlv) + 4) + tlv


class GattSimulator:
    """Strict image-0 peripheral that sends fragmented raw SMP notifications."""
    def __init__(self, image, public):
        self.public = public
        self.active = image
        self.secondary = b''
        self.written = bytearray()
        self.upload_sha = None
        self.pending = False
        self.permanent = False
        self.confirmed = True
        self.callbacks = {}
        self.commands = []
        self.resets = 0
        self.drop_at_offset = None
        self.drop_reset_reply = False
        self.corrupt_upload = False
        self.ignore_selection = False
        self.characteristic = SimpleNamespace(max_write_without_response_size=185)
        self.services = SimpleNamespace(get_characteristic=lambda _uuid: self.characteristic)

    def digest(self, data):
        return inspect_image(data, self.public).digest

    def state(self):
        result = {'images': [{'image': 0, 'slot': 0, 'hash': self.digest(self.active),
                              'active': True, 'confirmed': self.confirmed, 'bootable': True},
                             {'image': 1, 'slot': 0, 'hash': b'N' * 32, 'active': True,
                              'confirmed': True, 'bootable': True}]}
        if self.secondary:
            result['images'].append({'image': 0, 'slot': 1, 'hash': self.digest(self.secondary),
                                     'bootable': True, 'pending': self.pending, 'permanent': self.permanent})
        return result

    async def start_notify(self, _characteristic, callback):
        self.callback = callback

    async def write_gatt_char(self, _characteristic, packet, response):
        assert response is False
        assert len(packet) <= 185
        op, flags, length, group, sequence, command = struct.unpack_from('>BBHHBB', packet)
        assert flags == 0 and length == len(packet) - 8
        request = cbor2.loads(packet[8:])
        self.commands.append((op, group, command))
        if (op, group, command) == (0, 1, 0):
            result = self.state()
        elif (op, group, command) == (2, 1, 1):
            offset = request['off']
            if not offset:
                assert request['image'] == 0
                self.total = request['len']
                if request['sha'] != self.upload_sha:
                    self.written = bytearray()
                    self.secondary = b''
                    self.upload_sha = request['sha']
            if offset == len(self.written):
                self.written.extend(request['data'])
            elif offset != 0:
                raise AssertionError('Unexpected out-of-order write')
            if len(self.written) == self.total:
                assert hashlib.sha256(self.written).digest() == self.upload_sha
                if not self.corrupt_upload:
                    self.digest(bytes(self.written))
                    self.secondary = bytes(self.written)
            result = {'off': len(self.written)}
            if self.drop_at_offset is not None and offset >= self.drop_at_offset:
                self.drop_at_offset = None
                return  # Written, but ACK lost.
        elif (op, group, command) == (2, 1, 0):
            if request['hash'] == self.digest(self.active):
                assert request['confirm'] is True
                self.confirmed = True
            else:
                assert self.secondary and request['hash'] == self.digest(self.secondary)
                if not self.ignore_selection:
                    self.pending = True
                    self.permanent = request['confirm']
            result = self.state()
        elif (op, group, command) == (2, 0, 5):
            assert self.pending
            self.resets += 1
            self.active, self.secondary = self.secondary, self.active
            self.confirmed = self.permanent
            self.pending = False
            self.permanent = False
            result = {}
            if self.drop_reset_reply:
                self.drop_reset_reply = False
                return
        else:
            raise AssertionError(f'Unexpected operation: {(op, group, command)}')
        data = cbor2.dumps(result)
        reply = struct.pack('>BBHHBB', op + 1, 0, len(data), group, sequence, command) + data
        for start in range(0, len(reply), 7):
            self.callback(None, reply[start:start + 7])


class PipelineTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        cls.public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        cls.stock = signed_image(key, b'Stock firmware body' * 40)
        cls.cfw = signed_image(key, b'Custom firmware body' * 40)
        cls.stock_hash = inspect_image(cls.stock, cls.public).digest
        cls.cfw_hash = inspect_image(cls.cfw, cls.public).digest
        cls.profile = {'app_sha256': sha256(cls.stock), 'output_sha256': sha256(cls.cfw),
                       'stock_image_hash': cls.stock_hash.hex(), 'output_image_hash': cls.cfw_hash.hex()}

    async def peripheral(self):
        device = GattSimulator(self.stock, self.public)
        smp = SMP(device, timeout=0.02)
        await smp.start()
        return device, smp

    async def test_install_confirm_restore_at_wire_level(self):
        device, smp = await self.peripheral()
        await upload(smp, self.cfw, self.stock_hash, self.cfw_hash)
        self.assertEqual(device.active, self.stock)
        self.assertFalse(device.pending)
        await activate(smp, self.stock_hash, self.cfw_hash)
        self.assertEqual(device.active, self.cfw)
        self.assertFalse(device.confirmed)
        with self.assertRaisesRegex(FirmwareError, 'Confirm the current'):
            await upload(smp, self.stock, self.cfw_hash, self.stock_hash)
        await confirm(smp, self.cfw_hash)
        self.assertTrue(device.confirmed)
        await upload(smp, self.stock, self.cfw_hash, self.stock_hash)
        await activate(smp, self.cfw_hash, self.stock_hash, permanent=True)
        self.assertEqual(device.active, self.stock)
        self.assertTrue(device.confirmed)
        self.assertEqual(device.resets, 2)
        self.assertEqual(device.state()['images'][1]['hash'], b'N' * 32)

    async def test_lost_upload_ack_resumes_by_whole_file_hash(self):
        device, smp = await self.peripheral()
        device.drop_at_offset = 200
        with self.assertRaises(TimeoutError):
            await upload(smp, self.cfw, self.stock_hash, self.cfw_hash)
        self.assertGreater(len(device.written), 200)
        self.assertFalse(device.pending)
        resumed = SMP(device, timeout=0.02)
        await resumed.start()
        await upload(resumed, self.cfw, self.stock_hash, self.cfw_hash)
        self.assertEqual(device.secondary, self.cfw)

    async def test_lost_reset_ack_does_not_cause_second_reset(self):
        device, smp = await self.peripheral()
        await upload(smp, self.cfw, self.stock_hash, self.cfw_hash)
        device.drop_reset_reply = True
        with self.assertRaises(TimeoutError):
            await activate(smp, self.stock_hash, self.cfw_hash)
        result = await activate(smp, self.stock_hash, self.cfw_hash)
        self.assertIn('already-active', result['status'])
        self.assertEqual(device.resets, 1)

    async def test_incomplete_selection_does_not_reset(self):
        device, smp = await self.peripheral()
        await upload(smp, self.cfw, self.stock_hash, self.cfw_hash)
        device.ignore_selection = True
        with self.assertRaisesRegex(FirmwareError, 'pending state'):
            await activate(smp, self.stock_hash, self.cfw_hash)
        self.assertEqual(device.resets, 0)

    async def test_corrupt_uploaded_image_never_activates(self):
        device, smp = await self.peripheral()
        device.corrupt_upload = True
        with self.assertRaises(FirmwareError):
            await upload(smp, self.cfw, self.stock_hash, self.cfw_hash)
        self.assertNotIn((2, 1, 0), device.commands)
        self.assertEqual(device.resets, 0)

    async def test_resume_pending_selection_and_refuse_mode_change(self):
        device, smp = await self.peripheral()
        await upload(smp, self.cfw, self.stock_hash, self.cfw_hash)
        device.pending = True
        device.permanent = True
        with self.assertRaises(FirmwareError):
            await activate(smp, self.stock_hash, self.cfw_hash)
        await activate(smp, self.stock_hash, self.cfw_hash, permanent=True)
        self.assertEqual(device.resets, 1)
        self.assertNotIn((2, 1, 0), device.commands)

    async def test_wrong_current_or_confirmation_hash_never_writes(self):
        device, smp = await self.peripheral()
        with self.assertRaises(FirmwareError):
            await upload(smp, self.cfw, b'x' * 32, self.cfw_hash)
        with self.assertRaises(FirmwareError):
            await confirm(smp, self.cfw_hash)
        self.assertTrue(all(op == 0 for op, _, _ in device.commands))

    def test_plan_has_inverse_restore_path(self):
        install = plan_image(self.cfw, self.profile, self.public)
        restore = plan_image(self.stock, self.profile, self.public)
        self.assertIn(restore.target_hash, install.source_hashes)
        self.assertIn(install.target_hash, restore.source_hashes)
        self.assertEqual(restore.direction, 'restore-stock')
        with self.assertRaises(FirmwareError):
            plan_image(self.cfw[:-1], self.profile, self.public)

    def test_offline_cli_never_imports_bluetooth(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'image').write_bytes(self.cfw)
            (root / 'profile').write_text(json.dumps(self.profile))
            (root / 'public').write_bytes(self.public)
            with patch.object(g1flash, 'load_profiles', lambda: [self.profile]), patch.object(g1flash, 'PUBLIC_KEY', root / 'public'), \
                 patch.dict('sys.modules', {'bleak': None}), redirect_stdout(io.StringIO()) as output:
                self.assertEqual(g1flash.main(['plan', '--firmware', str(root / 'image')]), 0)
                self.assertEqual(json.loads(output.getvalue())['direction'], 'install-cfw')

    def test_parent_enforces_native_worker_deadline(self):
        with patch.object(g1flash.subprocess, 'run', side_effect=subprocess.TimeoutExpired('worker', 90)) as run, \
             redirect_stderr(io.StringIO()) as error:
            self.assertEqual(g1flash.main(['scan']), 1)
            self.assertEqual(run.call_args.kwargs['timeout'], 90)
            self.assertIn('deadline', error.getvalue())

    def test_invalid_firmware_is_rejected_before_worker_or_bluetooth(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'bad.bin'
            path.write_bytes(b'not firmware')
            with patch.object(g1flash.subprocess, 'run') as worker, redirect_stderr(io.StringIO()):
                self.assertEqual(g1flash.main(['stage', '--device', 'target', '--side', 'L',
                                              '--firmware', str(path)]), 1)
                worker.assert_not_called()

    async def test_dynamic_mtu_is_used_and_write_timeout_is_bounded(self):
        device, smp = await self.peripheral()
        device.characteristic.max_write_without_response_size = 20
        self.assertEqual(smp.limit, 20)
        device.characteristic.max_write_without_response_size = 185
        self.assertEqual(smp.limit, 185)
        async def stuck_write(*_args, **_kwargs):
            await asyncio.sleep(60)
        device.write_gatt_char = stuck_write
        with self.assertRaises(TimeoutError):
            await smp.state()

    def test_session_journal_preserves_evidence_and_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'session.jsonl'
            journal = g1flash.Journal(path)
            journal.event('reset-requested', {'hash': self.cfw_hash})
            journal.close()
            self.assertEqual(json.loads(path.read_text())['details']['hash'], self.cfw_hash.hex())
            with self.assertRaises(FileExistsError):
                g1flash.Journal(path)


class DiagnosticTests(unittest.IsolatedAsyncioTestCase):
    reply = b'G1CFW/1 net bt: net-date, app build time 2025-03-21 11:38:43, ver 1.5.60, JBD DeviceID ab12\n\r'

    async def test_fragmented_query_and_strict_stock_verification(self):
        class Client:
            services = SimpleNamespace(get_characteristic=lambda _uuid: SimpleNamespace(properties=['write']))
            stopped = False

            async def start_notify(self, char, callback):
                assert char == NUS_TX
                self.callback = callback

            async def write_gatt_char(self, _char, data, response):
                assert data == b'#t' and response
                for index in range(0, len(DiagnosticTests.reply), 3):
                    self.callback(None, DiagnosticTests.reply[index:index + 3])

            async def stop_notify(self, char):
                assert char == NUS_TX
                self.stopped = True

        client = Client()
        reply = await g1flash.query_marker(client)
        self.assertTrue(client.stopped)
        self.assertTrue(g1flash.verify_reply(reply, 'cfw')['cfw_marker'])
        stock = reply.replace(b'G1CFW/1 net bt:', b'net build time:')
        self.assertFalse(g1flash.verify_reply(stock, 'stock')['cfw_marker'])
        for invalid in (stock, reply.replace(b'1.5.60', b'1.6.6'), reply[:-2], b'G1CFW/1'):
            with self.assertRaises(FirmwareError):
                g1flash.verify_reply(invalid, 'cfw')

    async def test_unknown_device_never_connects(self):
        class Scanner:
            @staticmethod
            async def find_device_by_filter(matches, timeout):
                assert not matches(SimpleNamespace(address='target', name='Even G2_57_L_123ABC'),
                                   SimpleNamespace(local_name=None))
                return None

        args = SimpleNamespace(command='probe', device='target', side='L', scan_seconds=1)
        def client(*_args, **_kwargs):
            raise AssertionError('Must not connect')
        with self.assertRaises(FirmwareError):
            await g1flash.run(args, scanner_cls=Scanner, client_cls=client)


if __name__ == '__main__':
    unittest.main()
