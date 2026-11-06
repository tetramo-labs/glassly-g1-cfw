#!/usr/bin/env python3
"""Developer-only: compile the injected G1CFW code and write a pinned profile.

Needs clang and the public MCUboot development private key. Ordinary builds
replay the committed profile (blob bytes, patches, signature) without either.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from cfw.image import apply_profile, inspect_image, sha256  # noqa: E402
from tools.build_blob import CFLAGS, CLANG, compile_blob, thumb_branch  # noqa: E402
from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import padding  # noqa: E402

TARGET_PATH = ROOT / 'cfw/targets/g1-1.6.3.json'
KEY_SHA = '1fc912d30251b821f251e127d4daf7ba9338dd5c04e5af100abfb5b7c7d4c022'
SLOT_BASE, SLOT_END, TRAILER_SECTOR = 0xc000, 0xfc000, 0x1000
MARKER_OLD = b'net build time:'
STUB = bytes.fromhex('00207047')       # movs r0, #0; bx lr (also replaces tail calls)
CALL_STUB = bytes.fromhex('002000bf')  # movs r0, #0; nop (replaces BL calls)
BW_TEMPLATE = bytes.fromhex('00f00090')


def branch_target(code, addr):
    """Decode a Thumb-2 BL or B.W; callers must preserve tail-call semantics."""
    hw1, hw2 = struct.unpack('<HH', code)
    if (hw1 >> 11) != 0x1e or (hw2 & 0xd000) not in (0x9000, 0xd000):
        raise ValueError(f'not BL/B.W at {addr:#x}')
    s, j1, j2 = (hw1 >> 10) & 1, (hw2 >> 13) & 1, (hw2 >> 11) & 1
    i1, i2 = ~(j1 ^ s) & 1, ~(j2 ^ s) & 1
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((hw1 & 0x3ff) << 12) | ((hw2 & 0x7ff) << 1)
    if s:
        imm -= 1 << 25
    return addr + 4 + imm


def validate_target(stock, target):
    if sha256(stock) != target['app_sha256']:
        raise ValueError('Pinned stock application mismatch')
    for guard in target['guards']:
        off = guard['address'] - SLOT_BASE
        expected = bytes.fromhex(guard['expected'])
        if stock[off:off + len(expected)] != expected:
            raise ValueError(f"Stock ABI guard failed: {guard['name']} at {guard['address']:#x}")


def main():
    target = json.loads(TARGET_PATH.read_text())
    CARVES, STUBS, HOOKS, RAW = (target[k] for k in ('carves', 'stubs', 'hooks', 'raw'))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, default=ROOT / 'build/even-g1-1.6.3.zip')
    parser.add_argument('--key', type=Path, default=ROOT / 'build/keys/root-rsa-2048.pem')
    parser.add_argument('--source', type=Path, default=ROOT / 'cfw/src/g1cfw.c')
    parser.add_argument('--protocol', type=int, default=48)
    parser.add_argument('--out', type=Path, default=ROOT / 'profiles/g1-1.6.3-cfw.json')
    parser.add_argument('--upgrade-from', type=Path, action='append', default=[],
                        help='Older profile whose installed image may be upgraded in place')
    parser.add_argument('--carve', choices=sorted(CARVES), help='Place the blob over this stock function instead of appending')
    parser.add_argument('--stub', choices=sorted(set(STUBS) | set(RAW) | set(HOOKS)), action='append', default=[],
                        help='Also stub this group of stock functions (return 0 immediately)')
    args = parser.parse_args()
    package, pem = args.stock.read_bytes(), args.key.read_bytes()
    if sha256(package) != target['package_sha256'] or sha256(pem) != KEY_SHA:
        raise SystemExit('Pinned stock ZIP or MCUboot development key mismatch')
    with zipfile.ZipFile(args.stock) as archive:
        stock = archive.read('app_update.bin')
        net = archive.read('net_core_app_update.bin')
    validate_target(stock, target)
    if sha256(net) != target['network_sha256']:
        raise SystemExit('Pinned network image mismatch')
    public = (ROOT / 'profiles/mcuboot-development-public.pem').read_bytes()
    original = inspect_image(stock, public)

    import re
    revision = re.search(r'#define CFW_PROTOCOL (\d+)u', args.source.read_text())
    if not revision or int(revision[1]) != args.protocol:
        raise SystemExit('Protocol marker must match the source CFW_PROTOCOL')
    blob, funcs, rodata_len = compile_blob(args.source, ROOT / 'build/obj')
    entry = next(f for f in funcs if f['name'] == 'cfw_rx')
    budget = SLOT_END - TRAILER_SECTOR - SLOT_BASE
    patches = []
    if args.carve:
        carve = CARVES[args.carve]
        lo, hi = carve['region']
        helpers = []
        for helper in carve.get('relocated_helpers', []):
            data = bytes.fromhex(helper['bytes'])
            off = helper['address'] - SLOT_BASE
            assert stock[off:off + len(data)] == data
            helpers.append((helper, lo + len(blob)))
            blob += data
        if len(blob) > hi - lo:
            raise SystemExit(f'Injected blob ({len(blob)} bytes) exceeds the {carve["name"]} region ({hi - lo} bytes)')
        append = b''
        blob_addr = lo
        region_file = lo - SLOT_BASE
        patches.append({'offset': carve['stub'] - SLOT_BASE, 'old': stock[carve['stub'] - SLOT_BASE:carve['stub'] - SLOT_BASE + 4].hex(),
                        'new': STUB.hex(), 'description': f'{carve["name"]}: stub entry (return 0); body reused for CFW code'})
        patches.append({'offset': region_file, 'old': stock[region_file:region_file + len(blob)].hex(), 'new': blob.hex(),
                        'description': f'CFW blob over {carve["name"]} body at {lo:#x}'})
        for caller in carve.get('callers', []):
            site, callee = caller['address'], caller['target']
            off = site - SLOT_BASE
            code = stock[off:off + 4]
            assert code.hex() == caller['expected'] and branch_target(code, site) == callee
            replacement = CALL_STUB if struct.unpack('<HH', code)[1] & 0x4000 else STUB
            patches.append({'offset': off, 'old': code.hex(), 'new': replacement.hex(),
                            'description': f"caller {site:#x}: {caller['description']} returns 0"})
        for helper, destination in helpers:
            for site in helper['callers']:
                off = site - SLOT_BASE
                code = stock[off:off + 4]
                assert branch_target(code, site) == helper['address']
                branch = bytearray(code)
                thumb_branch(branch, 0, destination - site)
                patches.append({'offset': off, 'old': code.hex(), 'new': branch.hex(),
                                'description': f'relocate quick-note reset helper to {destination:#x}'})

    else:
        place = (original.signed_end + 3) & ~3
        append = bytes(place - original.signed_end) + blob
        blob_addr = SLOT_BASE + place
        if original.signed_end + len(append) + (len(stock) - original.signed_end) > budget:
            raise SystemExit('Injected blob does not fit before the MCUboot trailer sector')
    entry_addr = blob_addr + entry['offset']
    for group in args.stub:
        for addr, why in STUBS.get(group, []):
            off = addr - SLOT_BASE
            patches.append({'offset': off, 'old': stock[off:off + 4].hex(), 'new': STUB.hex(),
                            'description': f'stub {addr:#x}: {why}'})
        for addr, old_hex, new_hex, why in RAW.get(group, []):
            off = addr - SLOT_BASE
            assert stock[off:off + len(bytes.fromhex(old_hex))].hex() == old_hex, f'raw patch {addr:#x}'
            patches.append({'offset': off, 'old': old_hex, 'new': new_hex, 'description': f'raw {addr:#x}: {why}'})
        for addr, fn_name, why in HOOKS.get(group, []):
            fn = next(f for f in funcs if f['name'] == fn_name)
            off = addr - SLOT_BASE
            site = bytearray(BW_TEMPLATE)
            thumb_branch(site, 0, blob_addr + fn['offset'] - addr)
            patches.append({'offset': off, 'old': stock[off:off + 4].hex(), 'new': site.hex(),
                            'description': f'hook {addr:#x}: b.w {fn_name} at {blob_addr + fn["offset"]:#x}; {why}'})

    MARKER_OFFSET = target['marker_offset']
    HOOK_SITE, HOOK_OLD = target['hook_site'], bytes.fromhex(target['hook_old'])
    marker_new = (f'G1CFW/{args.protocol} net bt:' if args.protocol < 10 else f'G1CFW/{args.protocol} netbt:').encode()
    assert len(marker_new) == len(MARKER_OLD) and stock.count(MARKER_OLD) == 1
    assert stock[MARKER_OFFSET:MARKER_OFFSET + len(MARKER_OLD)] == MARKER_OLD
    patches.append({'offset': MARKER_OFFSET, 'old': MARKER_OLD.hex(), 'new': marker_new.hex(),
                    'description': f'Read-only #t reply: identify G1CFW/{args.protocol}'})
    hook_file = HOOK_SITE - SLOT_BASE
    assert stock[hook_file:hook_file + 4] == HOOK_OLD
    site = bytearray(HOOK_OLD)
    thumb_branch(site, 0, entry_addr - HOOK_SITE)
    patches.append({'offset': hook_file, 'old': HOOK_OLD.hex(), 'new': site.hex(),
                    'description': f'BLE receive entry: bl cfw_rx at {entry_addr:#x} instead of spec_ble_command_hook'})
    if append:
        old_size = struct.unpack_from('<I', stock, 12)[0]
        new_size = old_size + len(append)
        patches.append({'offset': 12, 'old': struct.pack('<I', old_size).hex(), 'new': struct.pack('<I', new_size).hex(),
                        'description': 'MCUboot header: image size includes the appended blob'})

    body = bytearray(stock[:original.signed_end])
    for patch in patches:
        body[patch['offset']:patch['offset'] + len(bytes.fromhex(patch['old']))] = bytes.fromhex(patch['new'])
    body += append
    digest = hashlib.sha256(body).digest()
    key = serialization.load_pem_private_key(pem, password=None)
    signature = key.sign(bytes(body), padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
    upgrade_from = set()
    for path in args.upgrade_from:
        previous = json.loads(path.read_text())
        upgrade_from.update([previous['output_image_hash'], previous['stock_image_hash']])
        upgrade_from.update(previous.get('upgrade_from_image_hashes', []))
    upgrade_from = sorted(upgrade_from)
    clang_version = subprocess.run([CLANG, '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    profile = {
        'name': f'G1CFW/{args.protocol} injected 0xF0 handler; stock 1.6.3',
        'stock_url': target['stock_url'], 'stock_cache': target['stock_cache'],
        'base_version': target['version'], 'base_build': target['build'],
        'package_sha256': target['package_sha256'], 'app_sha256': sha256(stock), 'network_sha256': sha256(net),
        'stock_image_hash': original.digest.hex(), 'output_sha256': None, 'output_image_hash': digest.hex(),
        'key_hash': original.key_hash.hex(), 'upgrade_from_image_hashes': upgrade_from,
        'max_image_bytes': budget, 'patches': patches, 'append': append.hex(), 'signature': signature.hex(),
        'blob': {'source': str(args.source.relative_to(ROOT)), 'source_sha256': sha256(args.source.read_bytes()),
                 'address': blob_addr, 'entry': entry_addr, 'bytes': len(blob), 'rodata_bytes': rodata_len,
                 'placement': f'carve:{args.carve}' if args.carve else 'append', 'stubs': args.stub,
                 'region_bytes': (CARVES[args.carve]['region'][1] - CARVES[args.carve]['region'][0]) if args.carve else budget - (len(stock) - len(blob)),
                 'functions': funcs, 'compiler': clang_version, 'cflags': CFLAGS,
                 'abi_sha256': sha256((ROOT / 'cfw/src/fw_abi.h').read_bytes()),
                 'target_sha256': sha256(TARGET_PATH.read_bytes())},
    }
    tlvs = b''.join(struct.pack('<BBH', kind, 0, len(v)) + v for kind, v in
                    [(k, {0x01: original.key_hash, 0x10: digest, 0x20: signature}[k])
                     for k in sorted(original.tlvs, key=lambda k: original.tlvs[k][0])])
    result = bytes(body) + struct.pack('<HH', 0x6907, 4 + len(tlvs)) + tlvs
    profile['output_sha256'] = sha256(result)
    assert apply_profile(stock, profile, public) == result
    args.out.write_text(json.dumps(profile, indent=2) + '\n')
    print(json.dumps({'profile': str(args.out), 'blob_bytes': len(blob), 'blob_address': hex(blob_addr),
                      'placement': profile['blob']['placement'],
                      'entry': hex(entry_addr), 'image_bytes': len(result),
                      'headroom': profile['blob']['region_bytes'] - len(blob),
                      'output_sha256': profile['output_sha256'], 'output_image_hash': digest.hex()}, indent=2))


if __name__ == '__main__':
    main()
