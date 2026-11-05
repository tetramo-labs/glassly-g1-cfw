"""Regression checks for the 1.6.3 ABI, signed port, and host RAM diagnostics."""
import copy
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import unittest
from unittest.mock import AsyncMock
import zipfile

from build_cfw import DEFAULT_PROFILE, PUBLIC_KEY, ROOT, build, load_profiles
from cfw.image import FirmwareError, inspect_image, sha256
from cfw.plan import plan_image
from g1flash import CfwLink, CFW_PEEK, verify_reply
from tools.build_blob import CLANG, compile_blob, movwt_imm
from tools.gen_profile import TARGET_PATH, branch_target, validate_target

TARGET = json.loads(TARGET_PATH.read_text())
PROFILE = json.loads(DEFAULT_PROFILE.read_text())
STOCK = Path(os.environ.get('G1_STOCK_163_ZIP', ROOT / TARGET['stock_cache']))


class HostBaseTests(unittest.IsolatedAsyncioTestCase):
    async def test_ram_diagnostics_resolve_each_base_and_dynamic_state(self):
        for version, pointer, word, offset, imu in [
            ((1, 5, 60), 0x20006b9c, 0x20006b88, 0x109d, 0x20007c40),
            ((1, 6, 3), 0x20006bdc, 0x20006bc8, 0x10d4, 0x20008484),
        ]:
            link = object.__new__(CfwLink)
            ping = b'G1CFW/46\0' + bytes(version)
            link.check = AsyncMock(side_effect=[ping, struct.pack('<I', 0x20050000),
                                                struct.pack('<I', 70), b'\1', ping])
            self.assertEqual(await link.wear_state(), {'wear_word': 70, 'wear_detect': True})
            self.assertEqual((await link.stock_layout())['imu'], imu)
            reads = [(c.args[0], c.args[1]) for c in link.check.call_args_list if len(c.args) > 1]
            self.assertEqual(reads, [(CFW_PEEK, struct.pack('<IB', address, size))
                                    for address, size in [(pointer, 4), (word, 4), (0x20050000 + offset, 1)]])

    async def test_unknown_or_malformed_ping_never_reads_ram(self):
        for ping in [b'G1CFW/46\0\1\6\4', b'G1CFW/46', b'garbage\0\1\6\3', b'G1CFW/46\0\1\6']:
            link = object.__new__(CfwLink)
            link.check = AsyncMock(return_value=ping)
            with self.assertRaises(FirmwareError):
                await link.wear_state()
            self.assertEqual(link.check.await_count, 1)

    async def test_invalid_heap_pointer_stops_before_dereference(self):
        for pointer in [b'', struct.pack('<I', 0), struct.pack('<I', 0x20050001), struct.pack('<I', 0x2007ffff)]:
            link = object.__new__(CfwLink)
            link.check = AsyncMock(side_effect=[b'G1CFW/46\0\1\6\3', pointer])
            with self.assertRaises(FirmwareError):
                await link.wear_state()
            self.assertEqual(link.check.await_count, 2)

    def test_diagnostics_require_exact_version_build_pair(self):
        template = 'net build time: network, app build time {}, ver {}, JBD DeviceID abcd\r\n'
        for version, date in [('1.5.60', '2025-03-21 11:38:43'), ('1.6.3', '2025-06-28 17:28:42')]:
            self.assertEqual(verify_reply(template.format(date, version).encode(), 'stock')['version'], version)
            cfw = template.replace('net build time:', 'G1CFW/46 netbt:')
            self.assertTrue(verify_reply(cfw.format(date, version).encode(), 'cfw')['cfw_marker'])
        for version, date in [('1.6.3', '2025-03-21 11:38:43'), ('1.5.60', '2025-06-28 17:28:42'),
                              ('1.6.4', '2025-06-28 17:28:42')]:
            with self.assertRaises(FirmwareError):
                verify_reply(template.format(date, version).encode(), 'stock')


class PortMetadataTests(unittest.TestCase):
    def test_profile_matches_sources_and_abi_addresses(self):
        self.assertEqual(PROFILE['blob']['source_sha256'], sha256((ROOT / 'cfw/src/g1cfw.c').read_bytes()))
        header = (ROOT / 'cfw/src/fw_abi.h').read_bytes()
        self.assertEqual(PROFILE['blob']['abi_sha256'], sha256(header))
        self.assertEqual(PROFILE['blob']['target_sha256'], sha256(TARGET_PATH.read_bytes()))
        for guard in TARGET['guards']:
            if guard['name'].startswith('FW_'):
                match = re.search(r'#define ' + guard['name'] + r' .*?(0x[0-9a-f]+)', header.decode())
                self.assertEqual(int(match[1], 16), guard['address'] | 1, guard['name'])
        for suffix, (base, size) in zip(['', '2', '3'], TARGET['ram_regions']):
            for name, expected in [('BASE', base), ('SIZE', size)]:
                match = re.search(r'#define FW_CFW_RAM' + suffix + '_' + name + r' (0x[0-9a-f]+)', header.decode())
                self.assertEqual(int(match[1], 16), expected)
        base, size = TARGET['ram_regions'][1]
        self.assertEqual(base + size, 0x2001db40)  # Live stock word must remain outside CFW RAM.

    @unittest.skipUnless(shutil.which(CLANG), 'ARM clang required for source replay')
    def test_compiled_code_and_relocated_helper_match_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            blob, functions, _ = compile_blob(ROOT / 'cfw/src/g1cfw.c', Path(temp))
        helpers = TARGET['carves']['dashboard']['relocated_helpers']
        self.assertEqual(len(blob) % 4, 0)
        expected = blob + b''.join(bytes.fromhex(h['bytes']) for h in helpers)
        pinned = next(bytes.fromhex(p['new']) for p in PROFILE['patches'] if p['description'].startswith('CFW blob'))
        self.assertEqual(expected, pinned)
        self.assertEqual(functions, PROFILE['blob']['functions'])
        self.assertLess(len(expected), PROFILE['blob']['region_bytes'])


@unittest.skipUnless(STOCK.exists(), 'Supply G1_STOCK_163_ZIP for signed stock port regression')
class SignedPortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.package = STOCK.read_bytes()
        cls.public = PUBLIC_KEY.read_bytes()
        with zipfile.ZipFile(STOCK) as archive:
            cls.stock = archive.read('app_update.bin')
        cls.image, cls.archive = build(cls.package, PROFILE, cls.public)

    def test_signature_exact_patches_and_app_only_package(self):
        validate_target(self.stock, TARGET)
        stock = inspect_image(self.stock, self.public)
        cfw = inspect_image(self.image, self.public)
        self.assertEqual(len(self.image), 970085)
        self.assertEqual(cfw.signed_end, stock.signed_end)
        self.assertEqual(cfw.digest.hex(), PROFILE['output_image_hash'])
        allowed = set()
        for patch in PROFILE['patches']:
            start = patch['offset']
            allowed.update(range(start, start + len(bytes.fromhex(patch['new']))))
        changed = {i for i in range(stock.signed_end) if self.stock[i] != self.image[i]}
        self.assertTrue(changed <= allowed)
        with zipfile.ZipFile(io.BytesIO(self.archive)) as archive:
            self.assertEqual(set(archive.namelist()), {'app_update.bin', 'manifest.json'})
        with self.assertRaisesRegex(FirmwareError, 'Stock ZIP digest'):
            build(self.package + b'corrupt', PROFILE, self.public)
        broken = copy.deepcopy(TARGET)
        broken['guards'][0]['expected'] = '0000'
        with self.assertRaisesRegex(ValueError, 'Stock ABI guard failed'):
            validate_target(self.stock, broken)

    def test_call_stubs_and_quick_note_relocation(self):
        carve = TARGET['carves']['dashboard']
        for caller in carve['callers']:
            start = caller['address'] - 0xc000
            old = self.stock[start:start + 4]
            self.assertEqual(branch_target(old, caller['address']), caller['target'])
            is_bl = struct.unpack('<HH', old)[1] & 0x4000
            self.assertEqual(self.image[start:start + 4], bytes.fromhex('002000bf' if is_bl else '00207047'))
        helper = carve['relocated_helpers'][0]
        destination = PROFILE['blob']['address'] + PROFILE['blob']['bytes'] - len(bytes.fromhex(helper['bytes']))
        self.assertEqual(destination % 4, 0)
        for caller in helper['callers']:
            self.assertEqual(branch_target(self.image[caller - 0xc000:caller - 0xc000 + 4], caller), destination)
        offset = destination - 0xc000
        self.assertEqual(self.image[offset:offset + 12], bytes.fromhex(helper['bytes']))
        self.assertEqual(struct.unpack_from('<I', self.image, offset + 8)[0], 0x20004b20)

    def test_init_clear_hook_precedes_stock_led_enable(self):
        fn = next(f for f in PROFILE['blob']['functions'] if f['name'] == 'cfw_panel_init_clear')
        offset = 0x80d76 - 0xc000
        self.assertEqual(branch_target(self.image[offset:offset + 4], 0x80d76),
                         PROFILE['blob']['address'] + fn['offset'])
        self.assertEqual(struct.unpack_from('<HH', self.image, offset)[1] & 0xd000, 0x9000)
        # Stock init calls the replacement after reset/write setup, before its
        # C0/power sequence and A3. Guard the entire intervening instruction run.
        call = 0x80e50 - 0xc000
        self.assertEqual(branch_target(self.stock[call:call + 4], 0x80e50), 0x80d76)
        self.assertEqual(self.stock[0x80eae - 0xc000:0x80eb0 - 0xc000], bytes.fromhex('a320'))
        guard = next(g for g in TARGET['guards'] if g['name'] == 'panel_init clear before LED enable')
        self.assertLessEqual(guard['address'], 0x80e50)
        self.assertGreaterEqual(guard['address'] + len(bytes.fromhex(guard['expected'])), 0x80eb4)

    def test_naked_detours_replay_stock_stack_and_resume(self):
        # The 1.6.3 checker saves six registers; replaying the old eight corrupts the return stack.
        cases = [('cfw_work_mode_entry', 0x169c0, 2, 0x169c5),
                 ('cfw_work_mode_tick_entry', 0x297e0, 4, 0x297e5),
                 ('cfw_suspend_entry', 0x2d8f8, 2, 0x2d8ff)]
        for name, site, push_size, resume in cases:
            fn = next(f for f in PROFILE['blob']['functions'] if f['name'] == name)
            off = PROFILE['blob']['address'] - 0xc000 + fn['offset']
            code = self.image[off:off + fn['size']]
            self.assertEqual(code[:push_size], self.stock[site - 0xc000:site - 0xc000 + push_size])
            # Decode the final movw/movt r12; bx r12, allowing trailing alignment NOP.
            end = code.index(bytes.fromhex('6047'))
            lo = movwt_imm(*struct.unpack_from('<HH', code, end - 8))
            hi = movwt_imm(*struct.unpack_from('<HH', code, end - 4))
            self.assertEqual(lo | hi << 16, resume)

    def test_upgrade_from_previous_base_and_restore_current_base(self):
        profiles = load_profiles()
        install = plan_image(self.image, profiles, self.public)
        previous = json.loads((ROOT / 'profiles/g1-1.5.6-cfw.json').read_text())
        for source in [previous['stock_image_hash'], previous['output_image_hash'], PROFILE['stock_image_hash']]:
            self.assertIn(bytes.fromhex(source), install.source_hashes)
        restore = plan_image(self.stock, profiles, self.public)
        self.assertEqual(restore.direction, 'restore-stock')
        self.assertIn(bytes.fromhex(PROFILE['output_image_hash']), restore.source_hashes)
        historical = os.environ.get('G1_STOCK_ZIP')
        if historical:
            with zipfile.ZipFile(historical) as archive:
                old_stock = archive.read('app_update.bin')
            self.assertIn(bytes.fromhex(PROFILE['output_image_hash']), plan_image(old_stock, profiles, self.public).source_hashes)
