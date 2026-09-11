"""Decode a terrain capture written by the terrain-capture slice build.

Format (little-endian), one file per ProposalAction commit:
  char[4] "TPTG", u32 version (1)
  u8[0x80] Proposal tail +0x278..+0x2f8 (grid headers; vector pointers are meaningless offline)
  u8[0x70] Context (r9)
  u64 n, n bytes  height grid data  (CVec2f cells: height, base)
  u64 n, n bytes  material grid data (u8, 0xff = unchanged)
  u64 n, n bytes  mask grid words    (u32 words of a vector<bool>)
"""
import struct
import sys


def grid_hdr(tail, off):
    return struct.unpack_from("<4i", tail, off)


def main(path):
    b = open(path, "rb").read()
    if b[:4] != b"TPTG":
        sys.exit("not a TPTG capture")
    (ver,) = struct.unpack_from("<I", b, 4)
    p = 8
    tail = b[p:p + 0x80]; p += 0x80
    ctx = b[p:p + 0x70]; p += 0x70
    blobs = []
    for _ in range(3):
        (n,) = struct.unpack_from("<Q", b, p); p += 8
        blobs.append(b[p:p + n]); p += n
    hx0, hy0, hw, hh = grid_hdr(tail, 0x00)
    mx0, my0, mw, mh = grid_hdr(tail, 0x28)
    kx0, ky0, kw, kh = grid_hdr(tail, 0x50)
    (bits,) = struct.unpack_from("<Q", tail, 0x78)
    print(f"version {ver}")
    print(f"height grid   x0={hx0} y0={hy0} w={hw} h={hh} bytes={len(blobs[0])} (want {hw*hh*8})")
    print(f"material grid x0={mx0} y0={my0} w={mw} h={mh} bytes={len(blobs[1])} (want {mw*mh})")
    print(f"mask grid     x0={kx0} y0={ky0} w={kw} h={kh} words={len(blobs[2])//4} bits={bits} (want {kw*kh})")
    print("context:", ctx.hex(" ", 4))

    hb = blobs[0]
    if hw > 0 and hh > 0 and len(hb) == hw * hh * 8:
        cells = struct.unpack_from(f"<{hw*hh*2}f", hb)
        hts, bases = cells[0::2], cells[1::2]
        deltas = [a - c for a, c in zip(hts, bases)]
        changed = sum(1 for d in deltas if d != 0.0)
        print(f"cells {hw*hh}, changed {changed}")
        print(f"height min {min(hts):.3f} max {max(hts):.3f}; base min {min(bases):.3f} max {max(bases):.3f}")
        print(f"delta min {min(deltas):.3f} max {max(deltas):.3f}")
        # coarse picture of the delta, at most 60 columns
        step = max(1, (hw + 59) // 60)
        ramp = " .:-=+*#%@"
        dmax = max(abs(d) for d in deltas) or 1.0
        for y in range(0, hh, step):
            row = []
            for x in range(0, hw, step):
                d = deltas[y * hw + x]
                ch = ramp[min(9, int(abs(d) / dmax * 9.999))]
                row.append(ch if d >= 0 else ch.lower() if ch.isalpha() else ch)
            print("  " + "".join(row))
        cx, cy = hw // 2, hh // 2
        i = cy * hw + cx
        print(f"centre cell ({hx0+cx},{hy0+cy}): height {hts[i]:.4f} base {bases[i]:.4f}")

    mb = blobs[1]
    if mw > 0 and mh > 0 and len(mb) == mw * mh:
        hist = {}
        for v in mb:
            hist[v] = hist.get(v, 0) + 1
        print("material histogram:", dict(sorted(hist.items(), key=lambda kv: -kv[1])[:8]))
    kb = blobs[2]
    if kw > 0 and kh > 0 and len(kb) >= 4:
        words = struct.unpack_from(f"<{len(kb)//4}I", kb)
        setbits = sum(bin(w).count("1") for w in words)
        print(f"mask bits set {setbits} of {bits}")


if __name__ == "__main__":
    main(sys.argv[1])
