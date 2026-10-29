#!/usr/bin/env python3
"""Build the G1 marker MVP by replaying a pinned patch set. Never flashes."""
import argparse
import io
import json
from pathlib import Path
import urllib.request
import zipfile

from cfw.image import FirmwareError, apply_profile, inspect_image, sha256

ROOT = Path(__file__).resolve().parent
PROFILES = sorted((ROOT / 'profiles').glob('g1-*.json'))
PUBLIC_KEY = ROOT / 'profiles/mcuboot-development-public.pem'


def load_profiles():
    return [json.loads(path.read_text()) for path in PROFILES]


def build(package, profile, key):
    if sha256(package) != profile['package_sha256']:
        raise FirmwareError('Stock ZIP digest mismatch')
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        if len(archive.namelist()) != 3 or set(archive.namelist()) != {
            'app_update.bin', 'net_core_app_update.bin', 'manifest.json'
        }:
            raise FirmwareError('Unexpected stock archive contents')
        stock = archive.read('app_update.bin')
        network = archive.read('net_core_app_update.bin')
        manifest = json.loads(archive.read('manifest.json'))
    if sha256(network) != profile['network_sha256']:
        raise FirmwareError('Network image digest mismatch')
    inspect_image(network, key)
    patched = apply_profile(stock, profile, key)
    files = [row for row in manifest['files'] if row['file'] == 'app_update.bin']
    if len(files) != 1 or files[0]['load_address'] != 0xC200 or files[0]['image_index'] != '0':
        raise FirmwareError('Unexpected application manifest')
    manifest['files'] = files  # App only: never update the network core for this MVP.
    manifest['files'][0]['size'] = len(patched)
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED) as archive:
        for name, contents in [('app_update.bin', patched),
                               ('manifest.json', json.dumps(manifest, indent=2).encode() + b'\n')]:
            entry = zipfile.ZipInfo(name, date_time=(2026, 10, 28, 0, 0, 0))
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, contents)
    return patched, output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, help='Use an already downloaded stock ZIP')
    parser.add_argument('--profile', type=Path, action='append', help='Profile JSON (default: every profiles/g1-*.json)')
    parser.add_argument('--out', type=Path, default=ROOT / 'build')
    args = parser.parse_args()
    paths = args.profile or PROFILES
    profiles = [json.loads(path.read_text()) for path in paths]
    cache = args.out / 'g1-mvp/stock-firmware.zip'
    if args.stock:
        package = args.stock.read_bytes()
    elif cache.exists():
        package = cache.read_bytes()
    else:
        with urllib.request.urlopen(profiles[0]['stock_url'], timeout=60) as response:
            package = response.read(2 * 1024 * 1024 + 1)
    with zipfile.ZipFile(io.BytesIO(package)) as source:
        stock = source.read('app_update.bin')
    from cfw.plan import plan_image
    reports = {}
    for path, profile in zip(paths, profiles):
        # The marker profile keeps its historical output directory.
        out = args.out / ('g1-mvp' if path.stem.endswith('marker') else path.stem)
        image, archive = build(package, profile, PUBLIC_KEY.read_bytes())
        out.mkdir(parents=True, exist_ok=True)
        (out / 'app_update.bin').write_bytes(image)
        (out / 'g1-cfw.zip').write_bytes(archive)
        (out / 'stock-app_update.bin').write_bytes(stock)
        (out / 'stock-firmware.zip').write_bytes(package)
        plans = {name: plan_image(data, profiles, PUBLIC_KEY.read_bytes()).report()
                 for name, data in [('install', image), ('restore', stock)]}
        (out / 'flash-plans.json').write_text(json.dumps(plans, indent=2) + '\n')
        report = {
            'status': 'built-and-signature-verified',
            'profile': profile['name'], 'app_sha256': sha256(image),
            'image_hash': profile['output_image_hash'], 'zip_sha256': sha256(archive),
            'stock_zip_sha256': sha256(package), 'network_core_included': False,
            'stock_application_sha256': sha256(stock), 'image_bytes': len(image),
            'output_dir': str(out.relative_to(ROOT)),
        }
        (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        reports[path.stem] = report
    print(json.dumps(reports, indent=2))


if __name__ == '__main__':
    main()
