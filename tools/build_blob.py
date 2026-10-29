#!/usr/bin/env python3
"""Compile injected C to a position-independent Thumb-2 blob for the G1 app core.

Adapted from g2flash's build.py (same idea, retargeted to ARMv8-M mainline with
FPv5 and verified relocation families). The output is .text followed by any
referenced read-only data. Intra-blob BL/B.W and PC-relative movw/movt (-fropi)
relocations are resolved here; anything else is an error, so firmware entry
points must be called through absolute-constant function pointers.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

R_ARM_THM_CALL = 10
R_ARM_THM_JUMP24 = 30
R_ARM_THM_MOVW_PREL_NC = 49
R_ARM_THM_MOVT_PREL = 50
SHT_PROGBITS, SHT_REL, SHT_RELA = 1, 9, 4
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4

CLANG = os.environ.get('G1_CLANG', 'clang')
CFLAGS = [
    '--target=thumbv8m.main-none-eabi', '-mthumb', '-mcpu=cortex-m33', '-mfloat-abi=soft',
    '-Os', '-ffreestanding', '-fno-jump-tables', '-fomit-frame-pointer', '-fno-builtin',
    '-mno-unaligned-access', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
    '-fno-stack-protector', '-fropi', '-Wall', '-Wextra', '-Werror',
]


class BuildError(Exception):
    pass


def thumb_branch(blob, off, target):
    hw2_old = blob[off + 2] | (blob[off + 3] << 8)
    disp = target - (off + 4)
    if disp % 2 or not -(1 << 24) <= disp < (1 << 24):
        raise BuildError(f'branch at {off:#x} to {target:#x} out of range')
    imm = (disp >> 1) & 0xFFFFFF
    s, i1, i2 = (imm >> 23) & 1, (imm >> 22) & 1, (imm >> 21) & 1
    j1, j2 = (~(i1 ^ s)) & 1, (~(i2 ^ s)) & 1
    hw1 = 0xF000 | (s << 10) | ((imm >> 11) & 0x3FF)
    hw2 = (hw2_old & 0xD000) | (j1 << 13) | (j2 << 11) | (imm & 0x7FF)
    blob[off:off + 4] = struct.pack('<HH', hw1, hw2)


def movwt_imm(hw1, hw2):
    return ((hw1 & 0xF) << 12) | (((hw1 >> 10) & 1) << 11) | (((hw2 >> 12) & 7) << 8) | (hw2 & 0xFF)


def movwt_set(hw1, hw2, val):
    val &= 0xFFFF
    hw1 = (hw1 & ~((1 << 10) | 0xF)) | (((val >> 11) & 1) << 10) | ((val >> 12) & 0xF)
    hw2 = (hw2 & ~((7 << 12) | 0xFF)) | (((val >> 8) & 7) << 12) | (val & 0xFF)
    return hw1, hw2


def movwt(blob, off, sym, high):
    hw1, hw2 = struct.unpack_from('<HH', blob, off)
    addend = movwt_imm(hw1, hw2)
    if addend & 0x8000:
        addend -= 0x10000
    result = (sym + addend - off) & 0xFFFFFFFF
    val = (result >> 16) if high else result
    blob[off:off + 4] = struct.pack('<HH', *movwt_set(hw1, hw2, val))


def parse_elf(data):
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise BuildError('not ELF32-LE')
    (shoff,) = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    secs = []
    for i in range(shnum):
        fields = struct.unpack_from('<10I', data, shoff + i * shentsize)
        secs.append(dict(zip(('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'align', 'entsize'), fields)))
    names = secs[shstrndx]
    for s in secs:
        raw = data[names['offset'] + s['name']:]
        s['sname'] = raw[:raw.index(b'\0')].decode()
    return secs


def is_rodata(sec):
    return sec['type'] == SHT_PROGBITS and sec['flags'] & SHF_ALLOC and not sec['flags'] & (SHF_WRITE | SHF_EXECINSTR)


def compile_blob(src, obj_dir, defines=()):
    obj_dir.mkdir(parents=True, exist_ok=True)
    obj = obj_dir / (Path(src).stem + '.o')
    subprocess.run([CLANG, *CFLAGS, *defines, '-c', str(src), '-o', str(obj)], check=True)
    data = obj.read_bytes()
    secs = parse_elf(data)
    text_idx = next(i for i, s in enumerate(secs) if s['sname'] == '.text')
    text = secs[text_idx]
    for s in secs:
        if s['sname'] in ('.data', '.bss') and s['size']:
            raise BuildError(f'{src}: writable static data is not supported in a flash blob ({s["sname"]})')
    blob = bytearray(data[text['offset']:text['offset'] + text['size']])
    text_len = len(blob)
    base = {text_idx: 0}
    for i, s in enumerate(secs):
        if i != text_idx and is_rodata(s) and s['size']:
            while len(blob) % max(s['align'], 1):
                blob.append(0)
            base[i] = len(blob)
            blob += data[s['offset']:s['offset'] + s['size']]
    symtab = next(s for s in secs if s['sname'] == '.symtab')
    strtab = secs[symtab['link']]
    syms = []
    for i in range(symtab['size'] // 16):
        name, value, size, info, _other, shndx = struct.unpack_from('<IIIBBH', data, symtab['offset'] + i * 16)
        raw = data[strtab['offset'] + name:]
        syms.append(dict(name=raw[:raw.index(b'\0')].decode(), value=value, size=size, typ=info & 0xF, shndx=shndx))
    bad = []
    for rs in secs:
        if rs['type'] not in (SHT_REL, SHT_RELA) or rs['info'] not in base:
            continue
        ent = 12 if rs['type'] == SHT_RELA else 8
        for i in range(rs['size'] // ent):
            r_offset, r_info = struct.unpack_from('<II', data, rs['offset'] + i * ent)
            r_type, sym = r_info & 0xFF, syms[r_info >> 8]
            pos = base[rs['info']] + r_offset
            if r_type in (R_ARM_THM_CALL, R_ARM_THM_JUMP24) and rs['info'] == text_idx and sym['shndx'] == text_idx:
                thumb_branch(blob, pos, sym['value'] & ~1)
            elif r_type in (R_ARM_THM_MOVW_PREL_NC, R_ARM_THM_MOVT_PREL) and sym['shndx'] in base:
                movwt(blob, pos, base[sym['shndx']] + sym['value'], r_type == R_ARM_THM_MOVT_PREL)
            else:
                bad.append(f'{rs["sname"]}+{r_offset:#x} type {r_type} -> {sym["name"]!r}')
    if bad:
        raise BuildError(f'{src}: unresolvable relocations:\n  ' + '\n  '.join(bad))
    funcs = sorted((s['name'], s['value'] & ~1, s['size']) for s in syms if s['typ'] == 2 and s['shndx'] == text_idx)
    resolved = []
    for i, (name, value, size) in enumerate(funcs):
        if not size:
            size = (funcs[i + 1][1] if i + 1 < len(funcs) else text_len) - value
        resolved.append({'name': name, 'offset': value, 'size': size})
    while len(blob) % 4:
        blob.append(0)
    return bytes(blob), resolved, len(blob) - text_len


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('src', type=Path)
    parser.add_argument('--obj-dir', type=Path, default=Path('build/obj'))
    parser.add_argument('-D', action='append', default=[], dest='defines')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    try:
        blob, funcs, rodata_len = compile_blob(args.src, args.obj_dir, [f'-D{d}' for d in args.defines])
    except BuildError as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({'src': str(args.src), 'text': blob.hex(), 'rodata_len': rodata_len, 'functions': funcs}))
        return 0
    print(f'{args.src}: {len(blob)} bytes ({rodata_len} rodata)')
    for f in funcs:
        print(f'  {f["name"]:<32} +{f["offset"]:#06x} {f["size"]} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
