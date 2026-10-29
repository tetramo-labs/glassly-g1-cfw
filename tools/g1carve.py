#!/usr/bin/env python3
"""Report every way code outside a range enters it: bl/b.w targets, Thumb
function-pointer literals, and movw/movt pairs. Used to decide which entry
points need a stub before the range is reused for injected code."""
import argparse
import bisect
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from g1dis import Image  # noqa: E402
from capstone.arm import ARM_OP_IMM  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('lo', type=lambda x: int(x, 0))
    parser.add_argument('hi', type=lambda x: int(x, 0))
    parser.add_argument('--image', default='build/g1-mvp/stock-app_update.bin')
    parser.add_argument('--map', type=Path, default=Path('build/g1-1.5.60-functions.json'))
    args = parser.parse_args()
    img = Image(args.image)
    functions = json.loads(args.map.read_text())
    starts = [f['start'] for f in functions]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return functions[i] if i >= 0 else None

    inside = lambda a: args.lo <= (a & ~1) < args.hi
    entries = {}

    def note(target, source, how):
        if inside(target) and not inside(source):
            f = owner(source)
            entries.setdefault(target & ~1, []).append((source, how, f['name'] if f else None))

    pending = {}
    for insn in img.stream(img.base, img.end):
        m = insn.mnemonic
        if m in ('bl', 'blx', 'b', 'b.w') and insn.operands and insn.operands[0].type == ARM_OP_IMM:
            note(insn.operands[0].imm, insn.address, m)
        elif m == 'movw':
            pending[insn.operands[0].reg] = insn.operands[1].imm
        elif m == 'movt' and insn.operands[0].reg in pending:
            val = (insn.operands[1].imm << 16) | pending.pop(insn.operands[0].reg)
            note(val, insn.address, 'movw/movt')
    # Pointer literals anywhere in the image (pools, tables, .data init image).
    for off in range(img.hdr, len(img.raw) - 3, 4):
        word = int.from_bytes(img.raw[off:off + 4], 'little')
        if word & 1 and inside(word):
            note(word, img.addr(off), 'pointer')
    print(f'range {args.lo:#x}-{args.hi:#x} ({args.hi - args.lo} bytes): {len(entries)} external entry points')
    for target in sorted(entries):
        f = owner(target)
        name = f['name'] if f and f['start'] == target else (f'inside {f["name"]}+{target - f["start"]:#x}' if f and f['name'] else '?')
        srcs = ', '.join(f'{s:#x}({how}{" in " + n if n else ""})' for s, how, n in entries[target][:6])
        more = f' +{len(entries[target]) - 6} more' if len(entries[target]) > 6 else ''
        print(f'  {target:#07x} {name}: {srcs}{more}')
    internal = [f for f in functions if args.lo <= f['start'] < args.hi]
    print(f'functions inside: {len(internal)}; named: {[f["name"] for f in internal if f["name"]][:40]}')


if __name__ == '__main__':
    main()
