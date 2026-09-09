# TpF2 binary recon extractor — read-only analysis of TransportFever2.exe
# Produces organized text dumps for reverse-engineering work (Ghidra/x64dbg prep).
import re, os, collections

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\TransportFever2.exe"
OUT = os.path.dirname(os.path.abspath(__file__))

print("reading exe...")
data = open(EXE, "rb").read()
print("size:", len(data))

# ---- 1. full strings dump (offset + text), min len 6 ------------------------
print("scanning strings...")
strings = [(m.start(), m.group().decode("ascii"))
           for m in re.finditer(rb"[ -~]{6,}", data)]
print("strings found:", len(strings))
with open(os.path.join(OUT, "strings_all.txt"), "w", encoding="utf-8") as f:
    for off, s in strings:
        f.write(f"{off:08x}  {s}\n")

# ---- 2. RTTI class names ----------------------------------------------------
# MSVC decorated class names look like: .?AVClassName@Namespace1@Namespace2@@
# (namespaces listed innermost-first; templates contain ? and $)
print("extracting RTTI classes...")
rtti_re = re.compile(r"\.\?AV([A-Za-z0-9_?$@]+?)@@")
classes = set()
for _, s in strings:
    for m in rtti_re.finditer(s):
        classes.add(m.group(1))

def demangle(tok):
    parts = [p for p in tok.split("@") if p]
    parts.reverse()  # innermost-first -> outermost-first
    return "::".join(parts)

demangled = sorted(demangle(c) for c in classes)
with open(os.path.join(OUT, "rtti_classes.txt"), "w", encoding="utf-8") as f:
    f.write(f"# {len(demangled)} unique RTTI class names (demangled)\n")
    for d in demangled:
        f.write(d + "\n")
print("rtti classes:", len(demangled))

# ---- 3. assert strings: source path + following expression ------------------
print("extracting asserts...")
src_re = re.compile(r"^(?:[A-Za-z]:)?[\\/]?[A-Za-z0-9_\\/ .:-]+\.(cpp|h|hpp|c)$", re.IGNORECASE)
asserts = []
for i, (off, s) in enumerate(strings):
    if src_re.match(s):
        expr = strings[i + 1][1] if i + 1 < len(strings) else ""
        asserts.append((s, expr))
with open(os.path.join(OUT, "asserts.txt"), "w", encoding="utf-8") as f:
    f.write("# source file | (likely) assert expression that follows it in rodata\n")
    for path, expr in asserts:
        f.write(f"{path}  |  {expr}\n")
print("assert pairs:", len(asserts))

# ---- 4. source-file subsystem map -------------------------------------------
tree = collections.defaultdict(set)
for path, _ in asserts:
    norm = path.replace("/", "\\")
    marker = norm.lower().find("src\\")
    rel = norm[marker + 4:] if marker >= 0 else norm.lstrip("\\")
    parts = rel.split("\\")
    top = parts[0] if len(parts) > 1 else "(root)"
    tree[top].add(rel)
with open(os.path.join(OUT, "source_files.txt"), "w", encoding="utf-8") as f:
    for top in sorted(tree):
        f.write(f"\n== {top} ({len(tree[top])} files) ==\n")
        for p in sorted(tree[top]):
            f.write(f"  {p}\n")
print("subsystems:", len(tree))

# ---- 5. Lua binding surface: context windows around known API names ---------
# luaL_register-style tables store the name strings near each other in rodata.
print("extracting lua binding context...")
anchors = ["sendCommand", "bookJournalEntry", "setPlayer", "getComponent",
           "sendScriptEvent", "buildConstruction", "buyVehicle", "addPlayer",
           "getPlayerJournal", "setBulldozeable"]
seen_windows = []
anchor_hits = collections.defaultdict(int)
str_by_text = {}
for off, s in strings:
    if s in anchors:
        anchor_hits[s] += 1
        lo, hi = off - 2048, off + 2048
        win = [t for o, t in strings if lo <= o <= hi]
        seen_windows.append((s, off, win))

with open(os.path.join(OUT, "lua_api_windows.txt"), "w", encoding="utf-8") as f:
    for name, off, win in seen_windows:
        f.write(f"\n===== anchor '{name}' @ 0x{off:x} (+-2KB rodata window) =====\n")
        for t in win:
            f.write(f"  {t}\n")
print("anchor hits:", dict(anchor_hits))

# ---- 6. keyword evidence scan (MP / networking / command system) ------------
print("keyword evidence scan...")
keywords = ["multiplayer", "netcode", "lockstep", "replication", "desync",
            "steamnetworking", "steam lobby", "lobby", "enet", "raknet",
            "CommandList", "Command", "undo", "replay", "deterministic",
            "random seed", "seed"]
counts = collections.OrderedDict()
low = [(s.lower(), s) for _, s in strings]
for kw in keywords:
    k = kw.lower()
    hits = [orig for l, orig in low if k in l]
    counts[kw] = hits[:200]

with open(os.path.join(OUT, "keyword_evidence.txt"), "w", encoding="utf-8") as f:
    for kw, hits in counts.items():
        f.write(f"\n== '{kw}': {len(hits)} hit(s) (capped at 200) ==\n")
        for h in hits:
            f.write(f"  {h}\n")
print("done. files written to:", OUT)
