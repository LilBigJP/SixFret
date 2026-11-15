#!/usr/bin/env python3
"""Where the game calls a system function, with the code around each call.

Reads out/boot.bin (scripts/prepare.py) and needs capstone (`pip install capstone`):

    python tools/re_imports.py scePadOpenExt scePadOutputReport       # the call sites
    python tools/re_imports.py scePadReadState --before 40 --after 60 # more code around them
    python tools/re_imports.py --at 0xc9f78b --before 30 --after 30   # code around an address
    python tools/re_imports.py --function 0x1234                       # from an address to its ret

Addresses are offsets in the eboot image, the same as the runtime's "Caller return offset".
"""
import argparse
import re
import struct
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent


def load(path):
    raw = path.read_bytes()
    magic, size, entry, ns, nr, ni, flags = struct.unpack_from('<8s6Q', raw, 0)
    if magic != b'G3PROBE2':
        raise SystemExit('expected out/boot.bin from scripts/prepare.py')
    pos = 56
    segments = [struct.unpack_from('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    names = [raw[pos + i * 128:pos + i * 128 + 128].split(b'\0', 1)[0].decode() for i in range(ni)]
    pos += ni * 128
    relocs = [struct.unpack_from('<QQqq', raw, pos + i * 32) for i in range(nr)]
    pos += nr * 32
    return raw[pos:pos + size], segments, names, relocs


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('symbols', nargs='*')
    p.add_argument('--boot', type=Path, default=ROOT / 'out' / 'boot.bin')
    p.add_argument('--at', action='append', default=[], help='show code around this image offset')
    p.add_argument('--function', action='append', default=[], help='disassemble from this offset to the first ret')
    p.add_argument('--before', type=int, default=24, help='instructions shown before a site')
    p.add_argument('--after', type=int, default=24)
    p.add_argument('--max-sites', type=int, default=12)
    a = p.parse_args()
    image, segments, names, relocs = load(a.boot)
    symbol_of = dict(re.findall(r'\{"([^"#]+)#[^"]*","([^"]+)"\}',
                                (ROOT / 'src' / 'import_names.inc').read_text(encoding='utf-8')))
    code = [(v, v + m) for v, m, f in segments if f & 1]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    slots = {}  # GOT slot -> symbol
    for target, kind, index, addend in relocs:
        if kind:
            nid = names[index].split('#')[0]
            slots[target] = symbol_of.get(nid, names[index])
    stubs = {}  # PLT stub (jmp [rip+slot]) -> symbol
    for start, end in code:
        at = image.find(b'\xff\x25', start, end)
        while at >= 0:
            slot = at + 6 + struct.unpack_from('<i', image, at + 2)[0]
            if slot in slots:
                stubs[at] = slots[slot]
            at = image.find(b'\xff\x25', at + 1, end)

    def label(address):
        return stubs.get(address) or slots.get(address)

    def show(lo, hi, mark=None):
        for ins in md.disasm(image[lo:hi], lo):
            note = ''
            if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
                note = label(int(ins.op_str, 16)) or ''
            m = re.search(r'rip ([+-]) (0x[0-9a-f]+)', ins.op_str)
            if m:
                ref = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                text = label(ref)
                if not text and 0 <= ref < len(image):
                    s = image[ref:ref + 48].split(b'\0', 1)[0]
                    if len(s) >= 4 and all(32 <= c < 127 for c in s):
                        text = '"' + s.decode() + '"'
                note = f'{ref:#x}' + (f' {text}' if text else '')
            print(f"{'>>' if ins.address == mark else '  '} {ins.address:8x}: {ins.mnemonic:8s} {ins.op_str}" + (f'   ; {note}' if note else ''))

    def around(site, before, after):
        # Instruction boundaries before a site: decode from a few candidates and keep one that lands on it.
        for back in range(before * 6, before * 6 + 16):
            lo = max(0, site - back)
            addresses = [i.address for i in md.disasm(image[lo:site + 16], lo)]
            if site in addresses:
                start = addresses[max(0, addresses.index(site) - before)]
                break
        else:
            start = site
        end, count = site, 0
        for ins in md.disasm(image[site:site + after * 15 + 16], site):
            end = ins.address + ins.size
            count += 1
            if count > after:
                break
        show(start, end, site)

    for wanted in a.symbols:
        targets = [s for s, n in stubs.items() if n == wanted]
        slot_targets = [s for s, n in slots.items() if n == wanted]
        sites = []
        for start, end in code:
            for opcode in (b'\xe8', b'\xe9'):
                at = image.find(opcode, start, end)
                while at >= 0:
                    if at + 5 + struct.unpack_from('<i', image, at + 1)[0] in targets:
                        sites.append(at)
                    at = image.find(opcode, at + 1, end)
            at = image.find(b'\xff\x15', start, end)  # call [rip+slot]
            while at >= 0:
                if at + 6 + struct.unpack_from('<i', image, at + 2)[0] in slot_targets:
                    sites.append(at)
                at = image.find(b'\xff\x15', at + 1, end)
        print(f'=== {wanted}: {len(sites)} call sites ' + ' '.join(f'{s:#x}' for s in sorted(sites)))
        for site in sorted(sites)[:a.max_sites]:
            print(f'--- {wanted} at {site:#x}')
            around(site, a.before, a.after)
    for text in a.at:
        site = int(text, 16)
        print(f'--- around {site:#x}')
        around(site, a.before, a.after)
    for text in a.function:
        start = int(text, 16)
        print(f'--- function at {start:#x}')
        end = start
        for ins in md.disasm(image[start:start + 0x4000], start):
            end = ins.address + ins.size
            if ins.mnemonic in ('ret', 'int3'):
                break
        show(start, end)


if __name__ == '__main__':
    main()
