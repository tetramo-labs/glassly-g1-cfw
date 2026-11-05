"""Offline identity checks for installing a CFW profile or restoring its stock base."""
from dataclasses import dataclass

from .image import FirmwareError, inspect_image, sha256


@dataclass(frozen=True)
class FlashPlan:
    direction: str
    profile_name: str
    file_hash: str
    source_hashes: tuple
    target_hash: bytes
    size: int

    def report(self):
        return {'direction': self.direction, 'profile': self.profile_name, 'image': 0, 'bytes': self.size,
                'file_sha256': self.file_hash,
                'accepted_active_image_hashes': [h.hex() for h in self.source_hashes],
                'target_image_hash': self.target_hash.hex(), 'network_core_updated': False,
                'hardware_validated': False}


def plan_image(data, profiles, public_pem):
    """Match the file against every pinned profile. Installing a CFW requires the
    exact stock image or an older pinned CFW to be active; restoring stock requires
    one of the pinned CFW images to be active."""
    if isinstance(profiles, dict):
        profiles = [profiles]
    digest = sha256(data)
    parsed = inspect_image(data, public_pem)
    installs = [p for p in profiles if digest == p['output_sha256']]
    stock_of = [p for p in profiles if digest == p['app_sha256']]
    if installs:
        profile = installs[0]
        if parsed.digest.hex() != profile['output_image_hash']:
            raise FirmwareError('Image digest disagrees with profile')
        sources = [profile['stock_image_hash']] + list(profile.get('upgrade_from_image_hashes', []))
        return FlashPlan('install-cfw', profile.get('name', 'cfw'), digest, tuple(bytes.fromhex(h) for h in sources),
                         parsed.digest, len(data))
    if stock_of:
        if parsed.digest.hex() != stock_of[0]['stock_image_hash']:
            raise FirmwareError('Image digest disagrees with profile')
        # A new-base CFW explicitly accepting an older stock hash also keeps
        # that stock image available as an OTA restore target after migration.
        stock_hashes = {p['stock_image_hash'] for p in stock_of}
        sources = sorted({p['output_image_hash'] for p in profiles
                          if p in stock_of or stock_hashes.intersection(p.get('upgrade_from_image_hashes', []))})
        return FlashPlan('restore-stock', 'stock base of ' + '; '.join(p.get('name', 'cfw') for p in stock_of), digest,
                         tuple(bytes.fromhex(h) for h in sources), parsed.digest, len(data))
    raise FirmwareError('Image is neither a pinned CFW nor its exact stock application')
