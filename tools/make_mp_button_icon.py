"""Generate the in-game toolbar button's icon: three players, drawn.

    python tools/make_mp_button_icon.py

Three people, each a round head over a half-disc torso, the middle one in front and
larger, with a transparent gap cut around it, each in a company colour: the first three
of the palette (style_sheet/mp_lockstep.lua FIRST), company 1 in front. The game's own
toolbar icons are white masks; this one is RGBA so the colours show. Drawn at the
towns/lines buttons' 68x68 (and 34x34) and written into the mod as
res/textures/ui/button/mp_multiplayer[@2x].tga.
"""
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "mod", "mp_lockstep_1", "res", "textures", "ui", "button")
SIZE = 68
SS = 8          # drawn this many times larger, then scaled down: smooth edges
BASE = 49       # the torsos' flat bottom, in icon pixels: 3 above true centre, so the
                # bottom-heavy glyph looks centred in the disk (it measured centred and looked low)
# companies 1, 2 and 3 (style_sheet/mp_lockstep.lua FIRST): front, left, right
FRONT, LEFT, RIGHT = (230, 25, 75), (0, 130, 200), (60, 180, 75)


def person(cx, head_r, torso_r, neck):
    """One person as a mask: a round head over a half-disc torso standing on BASE."""
    m = Image.new("L", (SIZE * SS, SIZE * SS), 0)
    d = ImageDraw.Draw(m)
    s = lambda v: v * SS
    # the torso: the top half of a disc whose centre sits on the base line
    d.pieslice([s(cx - torso_r), s(BASE - torso_r), s(cx + torso_r), s(BASE + torso_r)], 180, 360, fill=255)
    head_cy = BASE - torso_r - neck - head_r
    d.ellipse([s(cx - head_r), s(head_cy - head_r), s(cx + head_r), s(head_cy + head_r)], fill=255)
    return m


def build():
    side = dict(head_r=6.5, torso_r=11, neck=2.5)
    front = person(34, head_r=8.5, torso_r=15, neck=3)
    # the gap: the front person grown by ~3 px, cleared out of the two behind
    gap = front.filter(ImageFilter.MaxFilter(2 * 3 * SS + 1))
    icon = Image.new("RGBA", front.size, (0, 0, 0, 0))
    for cx, colour in ((17, LEFT), (51, RIGHT)):
        m = person(cx, **side)
        m.paste(0, (0, 0), gap)
        icon.paste(colour + (255,), (0, 0), m)
    icon.paste(FRONT + (255,), (0, 0), front)
    return icon.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    icon = build()
    os.makedirs(OUT, exist_ok=True)
    icon.save(os.path.join(OUT, "mp_multiplayer@2x.tga"))
    icon.resize((SIZE // 2, SIZE // 2), Image.LANCZOS).save(os.path.join(OUT, "mp_multiplayer.tga"))
    print("wrote", OUT, "bbox", icon.getbbox())


if __name__ == "__main__":
    main()
