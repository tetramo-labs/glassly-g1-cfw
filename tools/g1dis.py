#!/usr/bin/env python3
"""Disassembly and cross-reference helper for the exact G1 application binary.

Reads an MCUboot app_update.bin, maps it at the slot address, and offers
Thumb-2 disassembly, string search, and cross-reference discovery (literal-pool
pointers, movw/movt pairs, and bl/b.w targets). Analysis only; writes nothing.
"""
import argparse
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_REG_PC

SLOT_BASE = 0xc000


class Image:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        magic, _load, hdr, _ptlv, img = struct.unpack_from('<IIHHI', self.raw, 0)
        if magic != 0x96f3b83d:
            raise SystemExit('not an MCUboot image')
        self.hdr = hdr
        self.body_len = img
        self.base = SLOT_BASE + hdr
        self.end = self.base + img
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.md.detail = True

    def file_off(self, addr):
        return addr - SLOT_BASE

    def addr(self, file_off):
        return file_off + SLOT_BASE

    def bytes_at(self, addr, n):
        off = self.file_off(addr)
        return self.raw[off:off + n]

    def word(self, addr):
        return struct.unpack_from('<I', self.raw, self.file_off(addr))[0]

    def find_string(self, text):
        hits = []
        start = 0
        needle = text.encode() if isinstance(text, str) else text
        while True:
            i = self.raw.find(needle, start)
            if i < 0:
                return hits
            hits.append(self.addr(i))
            start = i + 1

    def disasm(self, addr, count=None, until=None):
        addr &= ~1
        code = self.raw[self.file_off(addr):self.file_off(until) if until else None]
        out = []
        for insn in self.md.disasm(code, addr):
            out.append(insn)
            if count and len(out) >= count:
                break
            if until and insn.address + insn.size >= until:
                break
        return out

    def stream(self, lo, hi):
        """Linear-sweep disassembly that steps past undecodable data words."""
        a = lo & ~1
        code = self.raw
        while a < hi:
            produced = False
            for insn in self.md.disasm(code[self.file_off(a):self.file_off(hi)], a):
                produced = True
                yield insn
                a = insn.address + insn.size
            if not produced:
                a += 2

    def literal_refs(self, value):
        """Addresses of aligned 32-bit words equal to value (candidate literal pools)."""
        hits = []
        packed = struct.pack('<I', value)
        for off in range(self.hdr, len(self.raw) - 3, 4):
            if self.raw[off:off + 4] == packed:
                hits.append(self.addr(off))
        return hits

    def xrefs(self, target, lo=None, hi=None):
        """Code locations referencing target: ldr-literal pools, movw/movt, bl/b.w."""
        lo = lo or self.base
        hi = hi or self.end
        pools = set(self.literal_refs(target) + self.literal_refs(target | 1))
        refs = []
        pending_movw = {}
        for insn in self.stream(lo, hi):
            m = insn.mnemonic
            if m in ('bl', 'blx', 'b', 'b.w') and insn.operands and insn.operands[0].type == ARM_OP_IMM:
                if insn.operands[0].imm & ~1 == target & ~1:
                    refs.append((insn.address, f'{m} {insn.op_str}'))
            elif m.startswith('ldr') and len(insn.operands) == 2 and insn.operands[1].type == ARM_OP_MEM \
                    and insn.operands[1].mem.base == ARM_REG_PC:
                pool = ((insn.address + 4) & ~3) + insn.operands[1].mem.disp
                if pool in pools:
                    refs.append((insn.address, f'{m} {insn.op_str}  ; [{pool:#x}] = {self.word(pool):#x}'))
            elif m == 'movw':
                pending_movw[insn.operands[0].reg] = (insn.address, insn.operands[1].imm)
            elif m == 'movt':
                reg = insn.operands[0].reg
                if reg in pending_movw:
                    a, low = pending_movw.pop(reg)
                    val = (insn.operands[1].imm << 16) | low
                    if val & ~1 == target & ~1:
                        refs.append((a, f'movw/movt {val:#x}'))
        return refs

    def function_start(self, addr):
        """Walk backwards from addr to a plausible Thumb function prologue (push {.., lr})."""
        a = addr & ~1
        for _ in range(4096):
            insns = self.disasm(a, count=1)
            if insns and insns[0].mnemonic in ('push', 'push.w') and 'lr' in insns[0].op_str:
                return a
            a -= 2
        return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='build/g1-mvp/stock-app_update.bin')
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('dis'); p.add_argument('addr', type=lambda x: int(x, 0)); p.add_argument('--count', type=int, default=40)
    p = sub.add_parser('str'); p.add_argument('text')
    p = sub.add_parser('xref'); p.add_argument('addr', type=lambda x: int(x, 0))
    p = sub.add_parser('strxref'); p.add_argument('text')
    p = sub.add_parser('words'); p.add_argument('addr', type=lambda x: int(x, 0)); p.add_argument('--count', type=int, default=16)
    args = parser.parse_args(argv)
    img = Image(args.image)
    if args.command == 'dis':
        for i in img.disasm(args.addr, count=args.count):
            print(f'{i.address:#08x}: {i.bytes.hex():<10} {i.mnemonic:<8} {i.op_str}')
    elif args.command == 'str':
        for a in img.find_string(args.text):
            print(f'{a:#08x} (file {img.file_off(a):#x})')
    elif args.command == 'xref':
        for a, what in img.xrefs(args.addr):
            print(f'{a:#08x}: {what}')
    elif args.command == 'strxref':
        for s in img.find_string(args.text):
            print(f'string at {s:#08x}:')
            for a, what in img.xrefs(s):
                fn = img.function_start(a)
                print(f'  {a:#08x}: {what}   (function ~{fn:#x})' if fn else f'  {a:#08x}: {what}')
    elif args.command == 'words':
        for k in range(args.count):
            a = args.addr + 4 * k
            print(f'{a:#08x}: {img.word(a):#010x}')


if __name__ == '__main__':
    main()
