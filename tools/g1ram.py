#!/usr/bin/env python3
"""List RAM addresses referenced only from dead code ranges, with the gap to
the next referenced RAM address as an upper bound on the object's size.
Dead ranges are given as LO-HI pairs; the function map provides names."""
import argparse
import bisect
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from g1dis import Image  # noqa: E402
from capstone.arm import ARM_OP_MEM, ARM_REG_PC  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ranges', nargs='+', help='dead code ranges LO-HI (hex)')
    parser.add_argument('--image', default='build/g1-mvp/stock-app_update.bin')
    parser.add_argument('--map', type=Path, default=Path('build/g1-1.5.60-functions.json'))
    parser.add_argument('--min-gap', type=int, default=256)
    args = parser.parse_args()
    dead = [tuple(int(x, 16) for x in r.split('-')) for r in args.ranges]
    img = Image(args.image)
    functions = json.loads(args.map.read_text())
    starts = [f['start'] for f in functions]
    refs = {}  # ram address -> set of (code address)
    for insn in img.stream(img.base, img.end):
        if insn.mnemonic.startswith('ldr') and len(insn.operands) == 2 and insn.operands[1].type == ARM_OP_MEM \
                and insn.operands[1].mem.base == ARM_REG_PC:
            pool = ((insn.address + 4) & ~3) + insn.operands[1].mem.disp
            if pool + 4 > img.end:
                continue
            w = img.word(pool)
            if 0x20000000 <= w < 0x20080000:
                refs.setdefault(w, set()).add(insn.address)
    # Also count .data/.bss pointer words inside RAM-range tables? Skip: code refs only.
    addrs = sorted(refs)
    is_dead = lambda a: any(lo <= a < hi for lo, hi in dead)
    rows = []
    for i, a in enumerate(addrs):
        users = refs[a]
        if all(is_dead(u) for u in users):
            nxt = addrs[i + 1] if i + 1 < len(addrs) else 0x20080000
            fnames = set()
            for u in users:
                k = bisect.bisect_right(starts, u) - 1
                fnames.add(functions[k]['name'] or f'{functions[k]["start"]:#x}')
            rows.append((a, nxt - a, sorted(fnames)))
    rows = [r for r in rows if r[1] >= args.min_gap]
    rows.sort(key=lambda r: -r[1])
    total = 0
    for a, gap, fnames in rows:
        total += gap
        print(f'{a:#010x} gap {gap:6d}  {", ".join(fnames)[:90]}')
    print(f'{len(rows)} candidates, {total} bytes upper bound')


if __name__ == '__main__':
    main()
