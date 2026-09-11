"""Offline round trip of the asset-brush wire format (no game needed).

Builds TPAS strokes in Python exactly as StashAssetsFromProposal lays them out,
wraps them in the inject-file text the mod writes ("rm <ids|->\n<base64>"),
and runs the slice's own ParseAssetStroke + Base64Decode, extracted verbatim from
native/src/slice_hook.cpp and compiled with MSVC, over them. Checks the parsed
groups, strings, matrices and removal ids, and that damaged files are refused.

    python tools/re/asset_stroke_test.py
"""
import base64, os, random, struct, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = open(os.path.join(REPO, "native", "src", "slice_hook.cpp"), encoding="utf-8").read()


def cut(start, end):
    a = SRC.index(start)
    b = SRC.index(end, a)
    return SRC[a:b]


# the pieces ParseAssetStroke needs: base64, the limits, the struct and the parser
code = "\n".join([
    cut("static const char B64_ALPHABET", "static void AppendTerrainBlob"),
    "static const uint64_t TERRAIN_MAX_BYTES = 64ull << 20;",
    cut("static const uint32_t  ASSET_MAX_MODELS", "struct AssetBlob"),
    cut("struct AssetModelSrc", "// The replay: asset_inject_"),
])
main = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
''' + code + r'''
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::string text(n, '\0'); fread(&text[0], 1, n, f); fclose(f);
    std::vector<int32_t> rm; std::vector<std::vector<AssetModelSrc>> recs;
    const char* bad = ParseAssetStroke(text, &rm, &recs);
    if (bad) { printf("BAD %s\n", bad); return 0; }
    printf("OK %zu", rm.size());
    for (int32_t id : rm) printf(" %d", id);
    printf("\n%zu\n", recs.size());
    for (auto& r : recs) {
        printf("%zu\n", r.size());
        for (auto& m : r) {
            float t[16]; memcpy(t, m.m, 64);
            printf("%s\t%s\t%.5f %.5f %.5f %.5f\n", m.model.c_str(), m.extra.c_str(), t[0], t[12], t[13], t[14]);
        }
    }
    return 0;
}
'''

tmp = tempfile.mkdtemp(prefix="asset_stroke_")
with open(os.path.join(tmp, "t.cpp"), "w", encoding="utf-8") as fh:
    fh.write(main)
vc = r'"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"'
subprocess.run(f'cmd /c "call {vc} >nul && cd /d {tmp} && cl /nologo /EHsc /O2 t.cpp >nul"', shell=True)
exe = os.path.join(tmp, "t.exe")
if not os.path.exists(exe):
    print("compile failed"); sys.exit(1)


def stroke(groups, nrm):
    out = b"TPAS" + struct.pack("<III", 1, len(groups), nrm)
    for g in groups:
        out += struct.pack("<I", len(g))
        for model, extra, mat in g:
            out += struct.pack("<H", len(model)) + model.encode() + struct.pack("<H", len(extra)) + extra.encode()
            out += struct.pack("<16f", *mat)
    return out


def run(text):
    p = os.path.join(tmp, "in.txt")
    with open(p, "wb") as fh:
        fh.write(text)
    return subprocess.run([exe, p], capture_output=True, text=True).stdout


fails = []
def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)

rnd = random.Random(7)
def mat(x, y, z):
    s = 0.8 + 0.4 * rnd.random()
    return [s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, x, y, z, 1]

groups = [
    [("tree/sugar_maple.mdl", "", mat(-4386.03, 11038.85, 89.87)), ("tree/sugar_maple.mdl", "", mat(-4380.5, 11041.25, 90.1))],
    [("vegetation/rocks/rock_large_01_with_a_very_long_name.mdl", "lod0", mat(10.5, -20.25, 3.0))],
]
blob = stroke(groups, 2)
out = run(b"rm 12845,14068\n" + base64.b64encode(blob))
lines = out.splitlines()
check("a valid stroke parses", lines and lines[0] == "OK 2 12845 14068", out[:80])
check("group and model counts", lines[1:3] == ["2", "2"] and lines[5] == "1", str(lines[1:6]))
check("model strings (short and >16 chars) and second string",
      lines[3].startswith("tree/sugar_maple.mdl\t\t") and lines[6].startswith("vegetation/rocks/rock_large_01_with_a_very_long_name.mdl\tlod0\t"), lines[6][:80])
t = [float(v) for v in lines[3].split("\t")[2].split()]
expect = struct.unpack("<4f", struct.pack("<4f", groups[0][0][2][0], -4386.03, 11038.85, 89.87))  # float32-rounded
check("matrix scale and translation survive", all(abs(a - b) < 1e-4 for a, b in zip(t, expect)), f"{t} vs {expect}")

out = run(b"rm -\n" + base64.b64encode(stroke(groups[:1], 0)))
check("no removals ('-')", out.startswith("OK 0\n1\n2\n"), out[:40])
check("CRLF after the removal line is accepted", run(b"rm 5\r\n" + base64.b64encode(blob)).startswith("OK 1 5"))
check("removal-only stroke (erase that empties groups)", run(b"rm 9,10\n" + base64.b64encode(stroke([], 2))).startswith("OK 2 9 10\n0"))

bads = [
    (b"nope\n" + base64.b64encode(blob), "malformed inject file"),
    (b"rm 1,x\n" + base64.b64encode(blob), "bad removal list"),
    (b"rm 1\n" + base64.b64encode(b"XXXX" + blob[4:]), "not a TPAS stroke"),
    (b"rm 1\n" + base64.b64encode(blob[:-10]), "truncated matrix"),
    (b"rm 1\n" + base64.b64encode(blob + b"\x00"), "trailing bytes"),
    (b"rm 1\n" + base64.b64encode(stroke([[("", "", mat(0, 0, 0))]], 0)), "bad model path"),
    (b"rm 1\n!!!!", "payload is not base64"),
]
for text, why in bads:
    got = run(text).strip()
    check(f"refused: {why}", got == f"BAD {why}", got)
print("FAILED: " + ", ".join(fails) if fails else "ALL OK")
sys.exit(1 if fails else 0)
