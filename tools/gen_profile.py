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

STOCK_SHA = '40f5839def1c00da3e0ffea9274dc0e04f249596d31787bd0c497be363cf6f8f'
KEY_SHA = '1fc912d30251b821f251e127d4daf7ba9338dd5c04e5af100abfb5b7c7d4c022'
SLOT_BASE = 0xc000
SLOT_END = 0xfc000
TRAILER_SECTOR = 0x1000
MARKER_OFFSET = 0x95cf9
MARKER_OLD = b'net build time:'
# 1.5.60 BLE receive entry (1.6.3: FUN_00023d38): `bl spec_ble_command_hook`.
HOOK_SITE = 0x2338e
HOOK_OLD = bytes.fromhex('ebf7cbfd')
# Stock code the CFW replaces. DashBoard_Reflash (0x399e0..0x3d7a4) is only
# entered at its start (from ui_DashBoard_task); a 4-byte stub there returns 0
# and the rest of the body becomes the injected blob (tools/g1carve.py).
CARVES = {
    'dashboard': {'stub': 0x399e0, 'region': (0x399e4, 0x3d7a4), 'name': 'DashBoard_Reflash'},
}
STUB = bytes.fromhex('00207047')  # movs r0, #0; bx lr
# Stock functions silenced so the stock display path never touches the frame
# buffer or panel again (g2flash's framebuffer lease, by removal). Each is only
# entered at its start (tools/g1carve.py) and its return value is unused.
STUBS = {
    'display': [
        (0x80d58, 'whole-frame clear and push used on screen exit'),
    ],
}
# Stock entry points redirected into blob functions with a B.W (same 4 bytes
# as a stub). The blob function receives the stock arguments unchanged.
HOOKS = {
    'display': [
        (0x4bee0, 'cfw_display_tick', 'display_reflash_handler: display-thread tick for CFW animation'),
    ],    'input': [
        (0x2ba48, 'cfw_touch_event', 'touch_key_thread signal helper: every gesture code passes through the CFW'),
    ],
}
BW_TEMPLATE = bytes.fromhex('00f0009000')[:4]  # b.w placeholder; thumb_branch keeps the B.W opcode bits
# Raw instruction patches (address, old bytes, new bytes, why).
RAW = {
    # Open link policy: let any central connect and stay connected while the
    # glasses are worn. Stock accepts only the bonded phone's address and drops
    # an unbonded central 30 s after connecting (tools/g1dis.py evidence in
    # docs/G1_CFW_MVP.md). Both are stock behaviour the CFW does not need.
    'link': [
        (0x18c8e, 'c0b1', '18e0', 'ancs_connected: cbz -> b, accept any central address'),
        (0x2c4be, '2df0cbfc', '00bf00bf', 'low_speed_peripheral_dispatch_thread: no 30 s unbonded disconnect'),
    ],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, default=ROOT / 'build/g1-mvp/stock-firmware.zip')
    parser.add_argument('--key', type=Path, default=ROOT / 'build/keys/root-rsa-2048.pem')
    parser.add_argument('--source', type=Path, default=ROOT / 'cfw/src/g1cfw.c')
    parser.add_argument('--protocol', type=int, default=2)
    parser.add_argument('--out', type=Path, default=ROOT / 'profiles/g1-1.5.6-cfw.json')
    parser.add_argument('--upgrade-from', type=Path, action='append', default=[],
                        help='Older profile whose installed image may be upgraded in place')
    parser.add_argument('--carve', choices=sorted(CARVES), help='Place the blob over this stock function instead of appending')
    parser.add_argument('--stub', choices=sorted(set(STUBS) | set(RAW) | set(HOOKS)), action='append', default=[],
                        help='Also stub this group of stock functions (return 0 immediately)')
    args = parser.parse_args()
    package, pem = args.stock.read_bytes(), args.key.read_bytes()
    if sha256(package) != STOCK_SHA or sha256(pem) != KEY_SHA:
        raise SystemExit('Pinned stock ZIP or MCUboot development key mismatch')
    with zipfile.ZipFile(args.stock) as archive:
        stock = archive.read('app_update.bin')
        net = archive.read('net_core_app_update.bin')
    public = (ROOT / 'profiles/mcuboot-development-public.pem').read_bytes()
    original = inspect_image(stock, public)

    blob, funcs, rodata_len = compile_blob(args.source, ROOT / 'build/obj')
    entry = next(f for f in funcs if f['name'] == 'cfw_rx')
    budget = SLOT_END - TRAILER_SECTOR - SLOT_BASE
    patches = []
    if args.carve:
        carve = CARVES[args.carve]
        lo, hi = carve['region']
        if len(blob) > hi - lo:
            raise SystemExit(f'Injected blob ({len(blob)} bytes) exceeds the {carve["name"]} region ({hi - lo} bytes)')
        append = b''
        blob_addr = lo
        region_file = lo - SLOT_BASE
        patches.append({'offset': carve['stub'] - SLOT_BASE, 'old': stock[carve['stub'] - SLOT_BASE:carve['stub'] - SLOT_BASE + 4].hex(),
                        'new': STUB.hex(), 'description': f'{carve["name"]}: stub entry (return 0); body reused for CFW code'})
        patches.append({'offset': region_file, 'old': stock[region_file:region_file + len(blob)].hex(), 'new': blob.hex(),
                        'description': f'CFW blob over {carve["name"]} body at {lo:#x}'})
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
    upgrade_from = [json.loads(p.read_text())['output_image_hash'] for p in args.upgrade_from]
    clang_version = subprocess.run([CLANG, '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    profile = {
        'name': f'G1CFW/{args.protocol} injected 0xF0 handler; stock CDN package labelled 1.5.6',
        'stock_url': 'https://cdn.evenreal.co/firmware/3adb8ebbd35c2343409d6d0c9fe6cbb9.zip',
        'package_sha256': STOCK_SHA, 'app_sha256': sha256(stock), 'network_sha256': sha256(net),
        'stock_image_hash': original.digest.hex(), 'output_sha256': None, 'output_image_hash': digest.hex(),
        'key_hash': original.key_hash.hex(), 'upgrade_from_image_hashes': upgrade_from,
        'max_image_bytes': budget, 'patches': patches, 'append': append.hex(), 'signature': signature.hex(),
        'blob': {'source': str(args.source.relative_to(ROOT)), 'source_sha256': sha256(args.source.read_bytes()),
                 'address': blob_addr, 'entry': entry_addr, 'bytes': len(blob), 'rodata_bytes': rodata_len,
                 'placement': f'carve:{args.carve}' if args.carve else 'append', 'stubs': args.stub,
                 'region_bytes': (CARVES[args.carve]['region'][1] - CARVES[args.carve]['region'][0]) if args.carve else budget - (len(stock) - len(blob)),
                 'functions': funcs, 'compiler': clang_version, 'cflags': CFLAGS},
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
