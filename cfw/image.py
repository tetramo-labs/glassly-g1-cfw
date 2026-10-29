"""Strict MCUboot inspection for the pinned G1 MVP; no device operations."""
from dataclasses import dataclass
import hashlib
import struct

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa


class FirmwareError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class Image:
    header_size: int
    body_size: int
    signed_end: int
    digest: bytes
    key_hash: bytes
    tlvs: dict


def inspect_image(data, public_pem):
    if not 32 <= len(data) <= 2 * 1024 * 1024:
        raise FirmwareError('Invalid image size')
    magic, load, header, protected, size, flags = struct.unpack_from('<IIHHII', data)
    if magic != 0x96F3B83D or header != 512 or load or protected or flags:
        raise FirmwareError('Unsupported G1 MCUboot header/layout')
    end = header + size
    if size < 8 or end + 4 > len(data):
        raise FirmwareError('Truncated image body')
    magic, total = struct.unpack_from('<HH', data, end)
    if magic != 0x6907 or total < 4 or end + total != len(data):
        raise FirmwareError('Invalid/truncated TLV area or unexpected trailing data')
    entries = {}
    pos = end + 4
    while pos < len(data):
        if pos + 4 > len(data):
            raise FirmwareError('Truncated TLV header')
        kind, reserved, length = struct.unpack_from('<BBH', data, pos)
        pos += 4
        if reserved or kind in entries or pos + length > len(data):
            raise FirmwareError('Malformed or duplicate TLV')
        entries[kind] = (pos, data[pos:pos + length])
        pos += length
    if set(entries) != {0x01, 0x10, 0x20}:
        raise FirmwareError('Expected exactly KEYHASH, SHA256 and RSA2048 TLVs')
    digest = hashlib.sha256(data[:end]).digest()
    if entries[0x10][1] != digest:
        raise FirmwareError('MCUboot body digest mismatch')
    key = serialization.load_pem_public_key(public_pem)
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 2048:
        raise FirmwareError('Expected RSA-2048 public key')
    der = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.PKCS1)
    key_hash = hashlib.sha256(der).digest()
    if entries[0x01][1] != key_hash:
        raise FirmwareError('Signing-key fingerprint mismatch')
    if len(entries[0x20][1]) != 256:
        raise FirmwareError('Invalid RSA signature size')
    try:
        key.verify(entries[0x20][1], data[:end],
                   padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                   hashes.SHA256())
    except InvalidSignature as exc:
        raise FirmwareError('RSA-PSS signature verification failed') from exc
    return Image(header, size, end, digest, key_hash, entries)


def apply_profile(stock, profile, public_pem):
    """Replay a pinned profile: in-place patches, an optional appended code
    blob before the TLV area, and the regenerated integrity TLVs."""
    if sha256(stock) != profile['app_sha256']:
        raise FirmwareError('Stock application does not match the pinned profile')
    original = inspect_image(stock, public_pem)
    appended = bytes.fromhex(profile.get('append', ''))
    body = bytearray(stock[:original.signed_end])
    ranges = []
    for patch in profile['patches']:
        offset = patch['offset']
        old, new = bytes.fromhex(patch['old']), bytes.fromhex(patch['new'])
        if not old or len(old) != len(new):
            raise FirmwareError('Only same-size patches are supported')
        stop = offset + len(old)
        if offset < 0 or stop > original.signed_end or any(offset < b and stop > a for a, b in ranges):
            raise FirmwareError('Overlapping or out-of-bounds patch')
        if offset < original.header_size and (offset, stop) != (12, 16):
            raise FirmwareError('Only the header image-size field may change')
        if stock[offset:stop] != old:
            raise FirmwareError(f'Expected-byte mismatch at {offset:#x}')
        body[offset:stop] = new
        ranges.append((offset, stop))
    body += appended
    size = struct.unpack_from('<I', body, 12)[0]
    if original.header_size + size != len(body):
        raise FirmwareError('Header image size does not match the patched body')
    digest = hashlib.sha256(body).digest()
    if digest.hex() != profile['output_image_hash']:
        raise FirmwareError('Patched image hash differs from the profile')
    signature = bytes.fromhex(profile['signature'])
    replacement = {0x01: original.key_hash, 0x10: digest, 0x20: signature}
    order = sorted(original.tlvs, key=lambda kind: original.tlvs[kind][0])
    tlvs = b''.join(struct.pack('<BBH', kind, 0, len(replacement[kind])) + replacement[kind] for kind in order)
    result = bytes(body) + struct.pack('<HH', 0x6907, 4 + len(tlvs)) + tlvs
    if sha256(result) != profile['output_sha256']:
        raise FirmwareError('Patched output digest mismatch')
    patched = inspect_image(result, public_pem)
    if patched.signed_end != original.signed_end + len(appended) or patched.digest != digest:
        raise FirmwareError('Unexpected patched image layout or identity')
    if len(result) > profile.get('max_image_bytes', len(result)):
        raise FirmwareError('Patched image exceeds the primary slot budget')
    return result
