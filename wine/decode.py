#!/usr/bin/env python3
"""Split a hidsniff log at MARK lines and summarize the vendor traffic per section.

  decode.py captures/session.log                 list sections with packet counts
  decode.py captures/session.log dpi_800         show packets of one section
  decode.py captures/session.log dpi_800 dpi_1600   byte-diff two sections
"""
import sys
from collections import OrderedDict

SHOW_OPS = {"SET_FEATURE", "GET_FEATURE", "OUT"}


def load(path):
    sections = OrderedDict(start=[])
    current = "start"
    for line in open(path):
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "MARK":
            current = parts[2]
            sections.setdefault(current, [])
            continue
        if len(parts) < 5 or parts[2] not in SHOW_OPS:
            continue
        data = bytes(int(x, 16) for x in parts[5:])
        if not data or data[0] != 0x07:  # vendor report only
            continue
        sections[current].append((float(parts[0]), parts[2], data))
    return sections


def trim(data):
    return data.rstrip(b"\x00") or data[:1]


def fmt(data):
    return " ".join(f"{b:02x}" for b in trim(data))


def unique(packets):
    seen, out = set(), []
    for _, op, data in packets:
        key = (op, trim(data))
        if key not in seen:
            seen.add(key)
            out.append((op, data))
    return out


def show(packets):
    for op, data in unique(packets):
        tag = ">>" if op != "GET_FEATURE" else "<<"
        print(f"  {tag} {fmt(data)}")


def diff(a, b):
    ua, ub = unique(a), unique(b)
    for i in range(max(len(ua), len(ub))):
        pa = ua[i][1] if i < len(ua) else b""
        pb = ub[i][1] if i < len(ub) else b""
        if trim(pa) == trim(pb):
            continue
        changed = [j for j in range(max(len(pa), len(pb)))
                   if (pa[j] if j < len(pa) else None) != (pb[j] if j < len(pb) else None)]
        print(f"packet #{i}: bytes differ at {changed}")
        print(f"  A {fmt(pa)}")
        print(f"  B {fmt(pb)}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sections = load(sys.argv[1])
    names = sys.argv[2:]
    if not names:
        for name, packets in sections.items():
            print(f"{name:24s} {len(packets):5d} packets, {len(unique(packets)):4d} unique")
    elif len(names) == 1:
        show(sections[names[0]])
    else:
        diff(sections[names[0]], sections[names[1]])


if __name__ == "__main__":
    main()
