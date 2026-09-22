"""The player name follows the Steam persona unless TYPED -- and one of the
menu's own random defaults (Adjective+Noun from NAME_ADJ/NAME_NOUN) is never a
typed name. Both rig instances sat on 'HappyDingo' / 'DaringOcelot' with
auto=0 while every log said 'steam persona: ComradeSilver' (2026-09-17).

Checks, from the source: the word lists parse; the rule exists at load and at
the Enter key; and a Python copy of the classifier agrees on known names.
    python tools/steam_name_default_test.py
"""
import re
from pathlib import Path

src = (Path(__file__).resolve().parents[1] / "native/src/menu_hook.cpp").read_text(encoding="utf-8")


def words(name):
    body = re.search(rf"static const char\* const {name}\[\] = \{{(.*?)\}};", src, re.S)[1]
    return re.findall(r'"([A-Za-z]+)"', body)


adj, noun = words("NAME_ADJ"), words("NAME_NOUN")
assert len(adj) >= 40 and len(noun) >= 40, (len(adj), len(noun))


def generated(n):
    return any(n.startswith(a) and n[len(a):] in noun for a in adj)


for n in ("HappyDingo", "DaringOcelot", "GiantDingo", "SolarWalrus", "RustyRabbit"):
    assert generated(n), n
for n in ("ComradeSilver", "james", "Happy", "Dingo", "HappyDingoX", "happydingo", ""):
    assert not generated(n), n

# the rule is applied where a name is classified: at load (auto=0 in the file) and on Enter
load = src.split("static void LoadNames()")[1].split("static void SaveNames()")[0]
assert "IsGeneratedName(g_username)" in load and "g_userAuto = true" in load, "LoadNames does not re-classify a generated default"
enter = src.split("else if (vk == VK_RETURN)")[1][:1200]
assert "IsGeneratedName(g_username)" in enter, "Enter on an untouched default would mark it typed"
fn = re.split(r"static bool IsGeneratedName\(const char\* n\)\s*\{", src)[1][:800]   # the definition, not the forward declaration
assert "NAME_ADJ" in fn and "NAME_NOUN" in fn and "strcmp(n + la, NAME_NOUN[b]) == 0" in fn
print(f"steam name default: ok -- {len(adj)} adjectives x {len(noun)} nouns; a generated default follows Steam at load and on Enter")
