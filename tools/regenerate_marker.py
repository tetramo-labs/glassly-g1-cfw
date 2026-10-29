#!/usr/bin/env python3
"""Developer-only: regenerate the marker profile with MCUboot's public test key."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cfw.image import inspect_image, sha256
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

ROOT = Path(__file__).resolve().parents[1]
STOCK_SHA = '40f5839def1c00da3e0ffea9274dc0e04f249596d31787bd0c497be363cf6f8f'
KEY_SHA = '1fc912d30251b821f251e127d4daf7ba9338dd5c04e5af100abfb5b7c7d4c022'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--key', type=Path, required=True)
    args = parser.parse_args()
    package, pem = args.stock.read_bytes(), args.key.read_bytes()
    if sha256(package) != STOCK_SHA or sha256(pem) != KEY_SHA:
        raise ValueError('Pinned stock ZIP or MCUboot public development key mismatch')
    with zipfile.ZipFile(args.stock) as archive:
        stock = archive.read('app_update.bin')
        net = archive.read('net_core_app_update.bin')
    public = (ROOT / 'profiles/mcuboot-development-public.pem').read_bytes()
    original = inspect_image(stock, public)
    key = serialization.load_pem_private_key(pem, password=None)
    result = bytearray(stock)
    offset = 0x95CF9
    old = b'net build time:'
    new = b'G1CFW/1 net bt:'
    if stock[offset:offset + len(old)] != old or stock.count(old) != 1:
        raise ValueError('Marker location is not the expected unique diagnostic string')
    result[offset:offset + len(new)] = new
    digest = hashlib.sha256(result[:original.signed_end]).digest()
    signature = key.sign(bytes(result[:original.signed_end]),
                         padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                         hashes.SHA256())
    patches = [{'offset': offset, 'old': old.hex(), 'new': new.hex(),
                'description': 'Read-only #t reply: identify G1CFW/1; preserve format and size'}]
    for kind, replacement, description in [(0x10, digest, 'Updated MCUboot SHA256'),
                                            (0x20, signature, 'Updated RSA-PSS signature')]:
        position, previous = original.tlvs[kind]
        result[position:position + len(replacement)] = replacement
        patches.append({'offset': position, 'old': previous.hex(), 'new': replacement.hex(),
                        'description': description})
    inspect_image(bytes(result), public)
    profile = {
        'name': 'G1CFW/1 diagnostic marker; stock CDN package labelled 1.5.6',
        'stock_url': 'https://cdn.evenreal.co/firmware/3adb8ebbd35c2343409d6d0c9fe6cbb9.zip',
        'package_sha256': STOCK_SHA, 'app_sha256': sha256(stock),
        'network_sha256': sha256(net), 'stock_image_hash': original.digest.hex(),
        'output_sha256': sha256(result), 'output_image_hash': digest.hex(),
        'key_hash': original.key_hash.hex(), 'patches': patches,
    }
    (ROOT / 'profiles/g1-1.5.6-marker.json').write_text(json.dumps(profile, indent=2) + '\n')
    print(profile['output_sha256'])


if __name__ == '__main__':
    main()
