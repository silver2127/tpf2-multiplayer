#!/usr/bin/env python3
"""Name the functions of the Linux TransportFever2 binary from its own strings.

The Linux counterpart of funcsig.py + func2src.py, without Ghidra. The Linux
build is stripped, but it is not blind: GCC's assert macros embed
__PRETTY_FUNCTION__ (the full demangled signature) and __FILE__, exactly as
MSVC's embed __FUNCSIG__ on Windows, and every PIE string reference is a
RIP-relative LEA. Function boundaries come from .eh_frame: GCC emits an FDE
for every function, so the unwind table is a complete, exact function list.

    python3 tools/re/elf_funcsig.py <TransportFever2> <outdir>

Writes to <outdir>:
    functions.csv  rva,size                                  every FDE
    xrefs.csv      func_rva,site_rva,kind,string             sig | src | str
    funcsig.csv    func_rva,size,nsigs,signature,source      one row per function
                   that references a signature; `signature` is the one it
                   references most (an inlined callee's assert can add others)

Addresses are RVAs: the file's virtual addresses, since a PIE links at 0.
At run time add the image base (tpf2_proxy.log prints it).

Needs pyelftools (apt install python3-pyelftools).
"""
import bisect
import collections
import csv
import os
import re
import struct
import sys

from elftools.elf.elffile import ELFFile

# DW_EH_PE encodings used by GCC on x86-64.
PE_ABSPTR, PE_UDATA4, PE_SDATA4, PE_UDATA8, PE_SDATA8 = 0x00, 0x03, 0x0B, 0x04, 0x0C
PE_PCREL, PE_DATAREL = 0x10, 0x30
PE_OMIT = 0xFF

# lea r64, [rip+disp32]: REX.W (48 or 4C), 8D, ModRM with mod=00 rm=101.
LEA_RIP = re.compile(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)

SRC_RE = re.compile(r"\.(cpp|cc|c|h|hpp|inl)$")
SIG_RE = re.compile(r"^[^()\n]*[A-Za-z_~][A-Za-z0-9_]*(::[A-Za-z_~<>=!+\-*/%&|^\[\] ]+)*\s*\(.*\)")


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.elf = ELFFile(self.f)
        self.secs = {s.name: s for s in self.elf.iter_sections()}

    def section(self, name):
        s = self.secs.get(name)
        if s is None:
            sys.exit(f"no {name} section")
        return s["sh_addr"], s.data()


def read_encoded(buf, off, enc, vaddr_of_off, datarel_base):
    """Decode one DW_EH_PE value at buf[off]. Returns (value, new_off)."""
    fmt = enc & 0x0F
    if fmt in (PE_UDATA4, PE_SDATA4):
        (v,) = struct.unpack_from("<I" if fmt == PE_UDATA4 else "<i", buf, off)
        n = 4
    elif fmt in (PE_ABSPTR, PE_UDATA8, PE_SDATA8):
        (v,) = struct.unpack_from("<Q" if fmt != PE_SDATA8 else "<q", buf, off)
        n = 8
    else:
        raise ValueError(f"unsupported pointer encoding 0x{enc:02x}")
    app = enc & 0x70
    if app == PE_PCREL:
        v += vaddr_of_off
    elif app == PE_DATAREL:
        v += datarel_base
    elif app != 0:
        raise ValueError(f"unsupported pointer application 0x{enc:02x}")
    return v & 0xFFFFFFFFFFFFFFFF, off + n


def functions_from_eh_frame(img):
    """Every FDE's (start, size), via .eh_frame_hdr's sorted search table."""
    hdr_addr, hdr = img.section(".eh_frame_hdr")
    eh_addr, eh = img.section(".eh_frame")
    version, ptr_enc, count_enc, table_enc = hdr[0], hdr[1], hdr[2], hdr[3]
    if version != 1 or count_enc == PE_OMIT or table_enc != (PE_DATAREL | PE_SDATA4):
        sys.exit(f".eh_frame_hdr layout not supported (v{version} enc {ptr_enc:02x} {count_enc:02x} {table_enc:02x})")
    off = 4
    _, off = read_encoded(hdr, off, ptr_enc, hdr_addr + off, hdr_addr)
    count, off = read_encoded(hdr, off, count_enc, hdr_addr + off, hdr_addr)

    cie_ptr_enc = {}   # CIE offset -> FDE pointer encoding (the 'R' augmentation)

    def cie_encoding(cie_off):
        if cie_off in cie_ptr_enc:
            return cie_ptr_enc[cie_off]
        p = cie_off + 4 + 4 + 1                                   # length, id, version
        aug_end = eh.index(b"\0", p)
        aug = eh[p:aug_end].decode()
        p = aug_end + 1
        for _ in range(2):                                        # code, data alignment (LEB128)
            while eh[p] & 0x80:
                p += 1
            p += 1
        p += 1                                                    # return address register (v1: a byte)
        enc = PE_ABSPTR
        if aug.startswith("z"):
            while eh[p] & 0x80:                                   # augmentation data length
                p += 1
            p += 1
            for ch in aug[1:]:
                if ch == "R":
                    enc = eh[p]; p += 1
                elif ch == "P":
                    penc = eh[p]; p += 1
                    _, p = read_encoded(eh, p, penc & 0x7F, eh_addr + p, 0)
                elif ch == "L":
                    p += 1
                elif ch in "S":
                    pass
                else:
                    raise ValueError(f"CIE augmentation {aug!r}")
        cie_ptr_enc[cie_off] = enc
        return enc

    funcs = []
    for i in range(count):
        e = off + 8 * i
        loc, _ = read_encoded(hdr, e, table_enc, hdr_addr + e, hdr_addr)
        fde_addr, _ = read_encoded(hdr, e + 4, table_enc, hdr_addr + e + 4, hdr_addr)
        p = fde_addr - eh_addr
        (length,) = struct.unpack_from("<I", eh, p)
        if length == 0xFFFFFFFF:
            raise ValueError("64-bit DWARF FDE")
        (cie_rel,) = struct.unpack_from("<I", eh, p + 4)
        cie_off = p + 4 - cie_rel
        enc = cie_encoding(cie_off)
        begin, q = read_encoded(eh, p + 8, enc, eh_addr + p + 8, 0)
        size, _ = read_encoded(eh, q, enc & 0x0F, 0, 0)
        if begin != loc:
            raise ValueError(f"FDE at 0x{fde_addr:x}: pc_begin 0x{begin:x} != table 0x{loc:x}")
        funcs.append((begin, size))
    funcs.sort()
    return funcs


def rodata_strings(img, minlen=4):
    """addr -> string for every NUL-terminated printable run in .rodata."""
    addr, data = img.section(".rodata")
    out = {}
    for m in re.finditer(rb"[\x20-\x7e\t]{%d,}\x00" % minlen, data):
        out[addr + m.start()] = m.group()[:-1].decode("ascii")
    return out


def classify(s):
    if SRC_RE.search(s) and "/" in s:
        return "src"
    if SIG_RE.match(s) and "::" in s:
        return "sig"
    return "str"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    exe, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    img = Image(exe)

    funcs = functions_from_eh_frame(img)
    starts = [f[0] for f in funcs]
    print(f"functions (FDEs): {len(funcs)}")

    strings = rodata_strings(img)
    print(f".rodata strings: {len(strings)}")

    text_addr, text = img.section(".text")
    xrefs = []
    unplaced = 0
    for m in LEA_RIP.finditer(text):
        site = text_addr + m.start()
        (disp,) = struct.unpack_from("<i", text, m.start() + 3)
        target = site + 7 + disp
        s = strings.get(target)
        if s is None:
            continue
        i = bisect.bisect_right(starts, site) - 1
        if i < 0 or site >= funcs[i][0] + funcs[i][1]:
            unplaced += 1
            continue
        xrefs.append((funcs[i][0], site, classify(s), s))
    print(f"string xrefs placed in a function: {len(xrefs)} (outside any FDE: {unplaced})")

    with open(os.path.join(outdir, "functions.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["rva", "size"])
        w.writerows((f"0x{a:x}", s) for a, s in funcs)

    with open(os.path.join(outdir, "xrefs.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["func_rva", "site_rva", "kind", "string"])
        w.writerows((f"0x{f:x}", f"0x{s:x}", k, t) for f, s, k, t in xrefs)

    sizes = dict(funcs)
    sigs = collections.defaultdict(collections.Counter)
    srcs = collections.defaultdict(collections.Counter)
    for f, _, kind, s in xrefs:
        if kind == "sig":
            sigs[f][s] += 1
        elif kind == "src":
            srcs[f][s] += 1
    with open(os.path.join(outdir, "funcsig.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["func_rva", "size", "nsigs", "signature", "source"])
        for f in sorted(sigs):
            sig = sigs[f].most_common(1)[0][0]
            src = srcs[f].most_common(1)[0][0] if srcs[f] else ""
            w.writerow([f"0x{f:x}", sizes[f], len(sigs[f]), sig, src])
    named = len(sigs)
    single = sum(1 for f in sigs if len(sigs[f]) == 1)
    print(f"functions with a signature: {named} ({single} reference exactly one)")
    print(f"functions with a source path: {len(srcs)}")


if __name__ == "__main__":
    main()
