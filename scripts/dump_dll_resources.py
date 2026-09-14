#!/usr/bin/env python3
"""Dump PE RT_RCDATA resource map (id -> size + SPIR-V check) from a DLL.

Usage (on Steam Deck):
  python3 scripts/dump_dll_resources.py "/home/deck/.local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll"

Stdlib only. Read-only: never modifies the DLL.
"""
import struct
import sys

SDBG = 0x07230203  # SPIR-V magic


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def parse(dll_path):
    with open(dll_path, "rb") as f:
        data = f.read()
    if data[0:2] != b"MZ":
        raise ValueError("not a DOS/PE file")
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("bad PE signature")
    coff = pe + 4
    num_sect = u16(data, coff + 2)
    opt_size = u16(data, coff + 16)
    opt = coff + 20
    magic = u16(data, opt)
    if magic == 0x20B:
        rsrc_dir_off = opt + 112 + 2 * 8
    elif magic == 0x10B:
        rsrc_dir_off = opt + 96 + 2 * 8
    else:
        raise ValueError(f"unknown optional magic {magic:#x}")
    rsrc_rva, rsrc_size = struct.unpack_from("<II", data, rsrc_dir_off)

    sects = []
    sh = opt + opt_size
    for _ in range(num_sect):
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, sh + 8)
        sects.append((vaddr, vsize, rawptr, rawsize))
        sh += 40

    def rva_to_file(rva):
        for vaddr, vsize, rawptr, rawsize in sects:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        raise ValueError(f"RVA {rva:#x} not in any section")

    rsrc_base = rva_to_file(rsrc_rva)

    def entries_at(level_off, count):
        out = []
        for i in range(count):
            rid, off = struct.unpack_from("<II", data, level_off + i * 8)
            out.append((rid, off))
        return out

    def dir_counts(doff):
        name_c = u16(data, doff + 12)
        id_c = u16(data, doff + 14)
        return name_c, id_c

    # root -> find RCDATA (10)
    name_c, id_c = dir_counts(rsrc_base)
    rcdata_off = None
    for rid, off in entries_at(rsrc_base + 16, name_c + id_c):
        if (rid & 0x7FFFFFFF) == 10 and off & 0x80000000:
            rcdata_off = rsrc_base + (off & 0x7FFFFFFF)
    if rcdata_off is None:
        raise ValueError("no RT_RCDATA directory")

    name_c, id_c = dir_counts(rcdata_off)
    results = []
    for rid, off in entries_at(rcdata_off + 16, name_c + id_c):
        res_id = rid & 0x7FFFFFFF
        if not (off & 0x80000000):
            continue
        lang_off = rsrc_base + (off & 0x7FFFFFFF)
        lname, lid = dir_counts(lang_off)
        if lid < 1 and lname < 1:
            continue
        _, doff = struct.unpack_from("<II", data, lang_off + 16)
        if doff & 0x80000000:
            continue
        data_rva, size = struct.unpack_from("<II", data, rsrc_base + (doff & 0x7FFFFFFF))
        try:
            foff = rva_to_file(data_rva)
            blob = data[foff:foff + min(size, 64)]
        except ValueError:
            results.append((res_id, size, False, []))
            continue
        words = struct.unpack("<%dI" % (len(blob) // 4), blob) if len(blob) >= 4 else ()
        is_spv = bool(words) and words[0] == SDBG
        results.append((res_id, size, is_spv, list(words[:8])))
    return results


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    try:
        results = parse(sys.argv[1])
    except Exception as e:
        print(f"ERROR: {e}")
        return 1
    print(f"RCDATA entries: {len(results)}")
    print(f"{'id':>6} {'size':>10}  spirv  head-words")
    for rid, size, is_spv, words in sorted(results):
        head = " ".join(f"{w:08x}" for w in words)
        print(f"{rid:>6} {size:>10}  {'YES' if is_spv else 'no ':<5} {head}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
