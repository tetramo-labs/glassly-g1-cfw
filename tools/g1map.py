#!/usr/bin/env python3
"""Build a function map of the exact G1 application binary.

One linear sweep collects Thumb-2 prologues (function starts) and literal-pool
loads. Functions are named from the log strings they reference: almost every
application function passes its own name to printk/ble_printk. Output is JSON
with start, end, size, and name (or null) per function, sorted by address.
"""
import argparse
import bisect
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from g1dis import Image  # noqa: E402
from capstone.arm import ARM_OP_MEM, ARM_REG_PC  # noqa: E402

NAME_RE = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{4,63}\0')


def sweep(img):
    starts = []
    loads = []  # (code address, pool address)
    for insn in img.stream(img.base, img.end):
        m = insn.mnemonic
        if m in ('push', 'push.w') and 'lr' in insn.op_str:
            starts.append(insn.address)
        elif m == 'stmdb' and 'sp!' in insn.op_str and 'lr' in insn.op_str:
            starts.append(insn.address)
        elif m.startswith('ldr') and len(insn.operands) == 2 and insn.operands[1].type == ARM_OP_MEM \
                and insn.operands[1].mem.base == ARM_REG_PC:
            loads.append((insn.address, ((insn.address + 4) & ~3) + insn.operands[1].mem.disp))
    return sorted(set(starts)), loads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='build/g1-mvp/stock-app_update.bin')
    parser.add_argument('--out', type=Path, default=Path('build/g1-1.5.60-functions.json'))
    args = parser.parse_args()
    img = Image(args.image)
    starts, loads = sweep(img)
    # Candidate name strings: identifier-like, NUL-terminated, in the image.
    names = {}
    for m in NAME_RE.finditer(img.raw):
        names[img.addr(m.start())] = m.group()[:-1].decode()
    # Pool word -> code addresses that load it.
    by_pool = {}
    for code, pool in loads:
        by_pool.setdefault(pool, []).append(code)
    # String address -> functions that reference it via a literal pool.
    ref_count = {}
    for pool, codes in by_pool.items():
        if pool + 4 > img.end:
            continue
        word = img.word(pool)
        if word in names:
            for code in codes:
                i = bisect.bisect_right(starts, code) - 1
                if i >= 0:
                    ref_count.setdefault(starts[i], {}).setdefault(names[word], 0)
                    ref_count[starts[i]][names[word]] += 1
    functions = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else img.end
        name = None
        if start in ref_count:
            # The most-referenced identifier in a function is its own name (log prefix).
            name = max(ref_count[start].items(), key=lambda kv: (kv[1], -len(kv[0])))[0]
        functions.append({'start': start, 'end': end, 'size': end - start, 'name': name})
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(functions, indent=1) + '\n')
    named = sum(1 for f in functions if f['name'])
    print(f'{len(functions)} functions, {named} named, written to {args.out}')


if __name__ == '__main__':
    main()
