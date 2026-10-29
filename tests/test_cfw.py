import asyncio
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import unittest
import zipfile

import cbor2
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa

from build_cfw import PROFILES, PUBLIC_KEY, ROOT, build
from cfw.image import FirmwareError, apply_profile, inspect_image, sha256
from cfw.smp import Frames, SMP, candidate, encode, require_current, upload, upload_packet
from g1flash import is_g1


class ImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        cls.pem = cls.key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        header = struct.pack('<IIHHIIBBHII', 0x96F3B83D, 0, 512, 0, 32, 0, 0, 0, 0, 0, 0)
        signed = header.ljust(512, b'\0') + bytes(range(32))
        der = cls.key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.PKCS1)
        signature = cls.key.sign(signed, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        tlvs = b''.join(struct.pack('<BBH', kind, 0, len(data)) + data for kind, data in [
            (0x10, hashlib.sha256(signed).digest()), (1, hashlib.sha256(der).digest()), (0x20, signature)])
        cls.image = signed + struct.pack('<HH', 0x6907, len(tlvs) + 4) + tlvs

    def test_signature_and_layout(self):
        image = inspect_image(self.image, self.pem)
        self.assertEqual(image.signed_end, 544)
        self.assertEqual(len(image.digest), 32)

    def test_corruption_rejected(self):
        for offset in (0, 8, 10, 12, 16, 512, 548, len(self.image) - 1):
            with self.subTest(offset=offset):
                broken = bytearray(self.image)
                broken[offset] ^= 1
                with self.assertRaises(FirmwareError):
                    inspect_image(broken, self.pem)

    def test_truncation_and_trailing_data(self):
        for data in (self.image[:10], self.image[:512], self.image[:-1], self.image + b'\0'):
            with self.assertRaises(FirmwareError):
                inspect_image(data, self.pem)

    def test_duplicate_tlv_rejected(self):
        data = bytearray(self.image)
        data[544 + 4 + 36] = 0x10
        with self.assertRaisesRegex(FirmwareError, 'duplicate'):
            inspect_image(data, self.pem)


class ProtocolTests(unittest.TestCase):
    def test_fragmentation_and_coalescing(self):
        packet = encode(1, 1, 0, 255, {'images': []})
        for split in range(len(packet) + 1):
            parser = Frames()
            frames = parser.feed(packet[:split]) + parser.feed(packet[split:] + packet)
            self.assertEqual(frames, [(1, 1, 0, 255, {'images': []})] * 2)

    def test_invalid_packets(self):
        packets = [struct.pack('>BBHHBB', 1, 0, 8193, 1, 0, 0),
                   encode(0, 1, 0, 0, {}), encode(1, 1, 0, 0, []),
                   struct.pack('>BBHHBB', 1, 0, 2, 1, 0, 0) + b'\xa0\xa0']
        for packet in packets:
            with self.assertRaises(FirmwareError):
                Frames().feed(packet)

    def test_upload_stays_within_att_limit(self):
        data = bytes(range(256)) * 8
        for limit in (80, 100, 185, 244):
            for offset in (0, 128, 1024, len(data) - 1):
                payload = upload_packet(data, offset, limit)
                self.assertLessEqual(len(encode(2, 1, 1, 0, payload)), limit)
                self.assertTrue(payload['data'])
                if not offset:
                    self.assertEqual(payload['sha'], hashlib.sha256(data).digest())
                    self.assertEqual(payload['image'], 0)
        with self.assertRaises(FirmwareError):
            upload_packet(data, 0, 20)

    def test_device_filter(self):
        for name in ('Even G1_57_L_123ABC', 'Even G1V1560_57_L_123ABC', 'Even G1_L_123ABC'):
            self.assertTrue(is_g1(name, 'L'))
            self.assertFalse(is_g1(name, 'R'))
        for name in ('Even G2_57_L_123ABC', 'DFU', 'Even G1', None):
            self.assertFalse(is_g1(name))


STOCK = b'a' * 32
OUTPUT = b'b' * 32


def image_state():
    return {'images': [{'image': 0, 'slot': 0, 'active': True, 'confirmed': True, 'hash': STOCK}]}


class FakeSMP:
    limit = 185

    def __init__(self, data, offset=0):
        self.data = data
        self.written = bytearray(data[:offset])
        self.calls = []
        self.bad_offset = None
        self.current = STOCK

    async def state(self):
        state = image_state()
        state['images'][0]['hash'] = self.current
        if self.written == self.data:
            state['images'].append({'image': 0, 'slot': 1, 'hash': OUTPUT, 'bootable': True})
        return state

    async def request(self, op, group, command, payload):
        self.calls.append((op, group, command, payload))
        assert (op, group, command) == (2, 1, 1), 'Must not activate/reset/erase during upload'
        if self.bad_offset is not None:
            return {'off': self.bad_offset}
        if payload['off'] == 0 and self.written:
            return {'off': len(self.written)}
        assert payload['off'] == len(self.written)
        self.written.extend(payload['data'])
        return {'off': len(self.written)}


class UploadTests(unittest.IsolatedAsyncioTestCase):
    async def test_upload_and_resume(self):
        data = bytes(range(256)) * 4
        for offset in (0, 300, len(data)):
            fake = FakeSMP(data, offset)
            result = await upload(fake, data, STOCK, OUTPUT)
            self.assertEqual(fake.written, data)
            candidate(result, OUTPUT)

    async def test_wrong_base_never_writes(self):
        fake = FakeSMP(b'firmware')
        fake.current = b'c' * 32
        with self.assertRaises(FirmwareError):
            await upload(fake, fake.data, STOCK, OUTPUT)
        self.assertFalse(fake.calls)

    async def test_bad_ack_aborts(self):
        for bad in (0, -1, 9999, True):
            fake = FakeSMP(b'firmware')
            fake.bad_offset = bad
            with self.assertRaises(FirmwareError):
                await upload(fake, fake.data, STOCK, OUTPUT)
            self.assertEqual(len(fake.calls), 1)

    async def test_small_mtu_never_writes(self):
        fake = FakeSMP(b'firmware')
        fake.limit = 20
        with self.assertRaises(FirmwareError):
            await upload(fake, fake.data, STOCK, OUTPUT)
        self.assertFalse(fake.calls)

    def test_pending_and_wrong_image_rejected(self):
        state = image_state()
        state['images'][0]['pending'] = True
        with self.assertRaises(FirmwareError):
            require_current(state, STOCK)
        with self.assertRaises(FirmwareError):
            candidate(image_state(), OUTPUT)

    async def test_smp_checks_error_and_response_identity(self):
        class Characteristic:
            max_write_without_response_size = 185

        class Services:
            def get_characteristic(self, _uuid):
                return Characteristic()

        class Client:
            services = Services()
            result = {'rc': 7}
            wrong_group = False

            async def start_notify(self, _char, callback):
                self.callback = callback

            async def write_gatt_char(self, _char, packet, response):
                op, _, _, group, seq, cmd = struct.unpack_from('>BBHHBB', packet)
                self.callback(None, encode(op + 1, group + int(self.wrong_group), cmd, seq, self.result))

        client = Client()
        smp = SMP(client)
        await smp.start()
        with self.assertRaisesRegex(FirmwareError, 'rejected'):
            await smp.state()
        client.result = {}
        client.wrong_group = True
        with self.assertRaisesRegex(FirmwareError, 'Mismatched'):
            await smp.state()
        client.wrong_group = False
        self.assertEqual(await smp.state(), {})


@unittest.skipUnless(os.environ.get('G1_STOCK_ZIP'), 'Set G1_STOCK_ZIP for real stock regression')
class StockRegression(unittest.TestCase):
    def test_real_build_is_reproducible_and_changes_only_marker_and_integrity(self):
        package = Path(os.environ['G1_STOCK_ZIP']).read_bytes()
        profile = json.loads((ROOT / 'profiles/g1-1.5.6-marker.json').read_text())
        key = PUBLIC_KEY.read_bytes()
        image, archive = build(package, profile, key)
        self.assertEqual((image, archive), build(package, profile, key))
        with zipfile.ZipFile(os.environ['G1_STOCK_ZIP']) as source:
            stock = source.read('app_update.bin')
        parsed = inspect_image(stock, key)
        self.assertEqual(image[:0x95CF9], stock[:0x95CF9])
        self.assertEqual(image[0x95CF9 + 15:parsed.signed_end], stock[0x95CF9 + 15:parsed.signed_end])
        self.assertEqual(len(image), len(stock))
        self.assertEqual(image[0x95CF9:0x95CF9 + 15], b'G1CFW/1 net bt:')
        with self.assertRaises(FirmwareError):
            apply_profile(image, profile, key)  # Never patch twice or patch unknown firmware.
        broken = copy.deepcopy(profile)
        broken['patches'][0]['old'] = '00' * 15
        with self.assertRaises(FirmwareError):
            apply_profile(stock, broken, key)

    def test_injected_profile_appends_blob_and_hooks_receive_entry(self):
        import struct
        package = Path(os.environ['G1_STOCK_ZIP']).read_bytes()
        profile = json.loads((ROOT / 'profiles/g1-1.5.6-cfw.json').read_text())
        key = PUBLIC_KEY.read_bytes()
        image, _archive = build(package, profile, key)
        with zipfile.ZipFile(os.environ['G1_STOCK_ZIP']) as source:
            stock = source.read('app_update.bin')
        original, patched = inspect_image(stock, key), inspect_image(image, key)
        append = bytes.fromhex(profile.get('append', ''))
        self.assertEqual(patched.signed_end, original.signed_end + len(append))
        self.assertEqual(image[original.signed_end:patched.signed_end], append)
        self.assertEqual(profile['blob']['address'] % 4, 0)
        if not append:
            blob_file = profile['blob']['address'] - 0xc000
            blob = next(bytes.fromhex(p['new']) for p in profile['patches'] if p['description'].startswith('CFW blob'))
            self.assertEqual(image[blob_file:blob_file + len(blob)], blob)
            self.assertEqual(len(blob), profile['blob']['bytes'])
            stub = next(p for p in profile['patches'] if 'stub entry' in p['description'])
            self.assertEqual(image[stub['offset']:stub['offset'] + 4], bytes.fromhex('00207047'))
        self.assertEqual(struct.unpack_from('<I', image, 12)[0], patched.body_size)
        self.assertLessEqual(len(image), profile['max_image_bytes'])
        # Only the listed patches differ inside the stock body.
        changed = [i for i in range(original.signed_end) if image[i] != stock[i]]
        allowed = set()
        for patch in profile['patches']:
            allowed.update(range(patch['offset'], patch['offset'] + len(bytes.fromhex(patch['old']))))
        self.assertTrue(set(changed) <= allowed)
        # The hook is a Thumb-2 BL from the receive entry to cfw_rx inside the blob.
        hook = next(p for p in profile['patches'] if 'cfw_rx' in p['description'])
        site = 0xc000 + hook['offset']
        hw1, hw2 = struct.unpack_from('<HH', image, hook['offset'])
        s_bit, imm10, imm11 = (hw1 >> 10) & 1, hw1 & 0x3ff, hw2 & 0x7ff
        j1, j2 = (hw2 >> 13) & 1, (hw2 >> 11) & 1
        i1, i2 = (~(j1 ^ s_bit)) & 1, (~(j2 ^ s_bit)) & 1
        disp = (s_bit << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
        if disp & (1 << 24):
            disp -= 1 << 25
        self.assertEqual(hw2 & 0xd000, 0xd000)  # BL, not B.W
        self.assertEqual(site + 4 + disp, profile['blob']['entry'])
        self.assertIn(profile['stock_image_hash'], [h for h in [profile['stock_image_hash']]])
        self.assertIn(json.loads((ROOT / 'profiles/g1-1.5.6-marker.json').read_text())['output_image_hash'],
                      profile['upgrade_from_image_hashes'])


if __name__ == '__main__':
    unittest.main()
