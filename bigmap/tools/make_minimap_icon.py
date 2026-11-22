"""Generate the minimap's toolbar button icon: a folded map with a pin, drawn.

    python tools/make_minimap_icon.py

A three-panel folded map with a location pin standing over the middle panel, a
transparent gap cut around the pin, in green (the multiplayer mod's company 3,
style_sheet/mp_lockstep.lua FIRST, so the two toolbar buttons match). RGBA, 68x68
like the game's towns/lines button icons. build.bat embeds the file in the DLL
(tools/embed_bin.ps1) and src/minimap.h writes it to
res/textures/ui/bigmap/minimap_button@2x.tga at game start.
"""
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "mod", "minimap", "minimap_button@2x.tga")
SIZE = 68
SS = 8          # drawn this many times larger, then scaled down: smooth edges
GREEN = (60, 180, 75)


def pts(points):
    return [(x * SS, y * SS) for x, y in points]


def build():
    m = Image.new("L", (SIZE * SS, SIZE * SS), 0)
    d = ImageDraw.Draw(m)
    # the map: three panels, folded alternately
    for panel in ([(8, 20), (24, 14), (24, 54), (8, 60)],
                  [(26, 14), (42, 20), (42, 60), (26, 54)],
                  [(44, 20), (60, 14), (60, 54), (44, 60)]):
        d.polygon(pts(panel), fill=255)
    # the pin: a gap ~3 px wide cut into the map around it, then the pin, then its hole
    pin = Image.new("L", m.size, 0)
    pd = ImageDraw.Draw(pin)
    pd.ellipse([25 * SS, 6 * SS, 43 * SS, 24 * SS], fill=255)
    pd.polygon(pts([(26.5, 19), (41.5, 19), (34, 36)]), fill=255)
    m.paste(0, (0, 0), pin.filter(ImageFilter.MaxFilter(2 * 3 * SS + 1)))
    m.paste(255, (0, 0), pin)
    d.ellipse([30.5 * SS, 11.5 * SS, 37.5 * SS, 18.5 * SS], fill=0)
    m = m.resize((SIZE, SIZE), Image.LANCZOS)
    icon = Image.new("RGBA", (SIZE, SIZE), GREEN + (0,))
    icon.putalpha(m)
    return icon


def main():
    icon = build()
    icon.save(OUT)
    print("wrote", OUT, "bbox", icon.getbbox())


if __name__ == "__main__":
    main()
