#!/usr/bin/env python3
"""mkiso.py - a minimal ISO 9660 CD image (one folder, 8.3 names) for getting files into a VM.

    python mkiso.py OUT.iso VOLUME_ID FILE...        (each FILE goes in the root as its uppercased name)

Enough for Windows 95/98: Level 1 names (8.3, A-Z 0-9 _), no Joliet, no subfolders. With an
AUTORUN.INF among the files, Windows runs it when the disc appears. Standard library only.
"""
import os, struct, sys, time

SECTOR = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(name_bytes, lba, size, is_dir, when):
    rec = bytes([0, 0]) + both32(lba) + both32(size) + when + bytes([2 if is_dir else 0, 0, 0]) + both16(1)
    rec += bytes([len(name_bytes)]) + name_bytes
    if len(rec) % 2:
        rec += b"\0"
    rec = bytes([len(rec)]) + rec[1:]
    return rec


def iso_name(path):
    base = os.path.basename(path).upper()
    stem, _, ext = base.partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
    if not stem or len(stem) > 8 or len(ext) > 3 or set(stem + ext) - ok:
        sys.exit(f"mkiso: {base} is not an 8.3 name of A-Z 0-9 _")
    return (stem + "." + ext + ";1").encode()


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out, volid, files = sys.argv[1], sys.argv[2].upper()[:32], sys.argv[3:]
    t = time.gmtime()
    when = bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])
    datestr = time.strftime("%Y%m%d%H%M%S", t).encode() + b"00\0"

    entries = sorted(((iso_name(f), open(f, "rb").read()) for f in files), key=lambda e: e[0])
    root_lba, first_file = 20, 21
    lba, placed = first_file, []
    for name, data in entries:
        placed.append((name, lba, data))
        lba += max(1, (len(data) + SECTOR - 1) // SECTOR)
    total = lba

    root = dir_record(b"\0", root_lba, SECTOR, True, when) + dir_record(b"\1", root_lba, SECTOR, True, when)
    for name, flba, data in placed:
        root += dir_record(name, flba, len(data), False, when)
    if len(root) > SECTOR:
        sys.exit("mkiso: too many files for one directory sector")
    root_rec = dir_record(b"\0", root_lba, SECTOR, True, when)   # the copy inside the volume descriptor

    pt_l = bytes([1, 0]) + struct.pack("<I", root_lba) + struct.pack("<H", 1) + b"\0\0"
    pt_m = bytes([1, 0]) + struct.pack(">I", root_lba) + struct.pack(">H", 1) + b"\0\0"

    pad = lambda s, n: s.encode().ljust(n, b" ") if isinstance(s, str) else s.ljust(n, b" ")
    pvd = bytes([1]) + b"CD001" + bytes([1, 0]) + pad("", 32) + pad(volid, 32) + bytes(8) + both32(total)
    pvd += bytes(32) + both16(1) + both16(1) + both16(SECTOR) + both32(len(pt_l))
    pvd += struct.pack("<I", 18) + struct.pack("<I", 0) + struct.pack(">I", 19) + struct.pack(">I", 0)
    pvd += root_rec + pad(volid, 128) + pad("OKAGENT", 128) + pad("MKISO.PY", 128) + pad("", 128)
    pvd += pad("", 37) + pad("", 37) + pad("", 37) + datestr + datestr + b"0" * 16 + b"\0" + b"0" * 16 + b"\0"
    pvd += bytes([1, 0])
    pvd = pvd.ljust(SECTOR, b"\0")
    term = (bytes([255]) + b"CD001" + bytes([1])).ljust(SECTOR, b"\0")

    with open(out, "wb") as f:
        f.write(bytes(16 * SECTOR))
        f.write(pvd)
        f.write(term)
        f.write(pt_l.ljust(SECTOR, b"\0"))
        f.write(pt_m.ljust(SECTOR, b"\0"))
        f.write(root.ljust(SECTOR, b"\0"))
        for name, flba, data in placed:
            f.seek(flba * SECTOR)
            f.write(data)
        f.seek(total * SECTOR - 1)
        f.write(b"\0")
    print(f"mkiso: {out}: {len(placed)} file(s), {total} sectors")


if __name__ == "__main__":
    main()
