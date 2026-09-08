#!/usr/bin/env python3
r"""Render the mod's in-game thumbnail and the logo exports from the vector source.

    python tools\make_mod_image.py

Writes:
  mod\mp_lockstep_1\image_00.tga   320x180 RGB, uncompressed -- what Transport
                                   Fever 2 shows in the mod list (the size and
                                   format every stock urbangames_* mod uses)
  docs\logo\tfmp.png               2048x2048 with the white ground
  docs\logo\tfmp_transparent.png   2048x2048, transparent outside the roundel
  docs\logo\tfmp.svg               true vector

The roundel is square and the thumbnail is 16:9, so it is centred on a dark
ground at 168 px, which leaves the ring clear of the top and bottom edges the
game's list clips. Everything comes from tools\draw_tfmp_network_logo.py; do not
hand-edit the outputs, re-run this.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from draw_tfmp_network_logo import draw_logo  # noqa: E402

from PIL import Image  # noqa: E402

THUMB_W, THUMB_H = 320, 180          # measured from the stock mods' image_00.tga
ROUNDEL_PX = 168                     # 6 px of air above and below the outer ring
GROUND = (18, 18, 20)                # near-black, warm-neutral; reads as the game's UI
BLUE = "#007BFF"


def main() -> None:
    logo_dir = REPO / "docs" / "logo"
    logo_dir.mkdir(parents=True, exist_ok=True)

    # 1. the exports (white ground + transparent), both from the vector source
    png_white, svg = draw_logo(logo_dir / "tfmp", size=2048, blue=BLUE)
    png_alpha, _ = draw_logo(logo_dir / "tfmp_transparent", size=2048, blue=BLUE, transparent=True)
    (logo_dir / "tfmp_transparent.svg").unlink(missing_ok=True)   # one SVG is enough
    print(f"wrote {png_white.relative_to(REPO)}, {png_alpha.relative_to(REPO)}, {svg.relative_to(REPO)}")

    # 2. the thumbnail: transparent roundel, downsampled, centred on the ground
    roundel = Image.open(png_alpha).convert("RGBA")
    roundel = roundel.resize((ROUNDEL_PX, ROUNDEL_PX), Image.Resampling.LANCZOS)
    thumb = Image.new("RGBA", (THUMB_W, THUMB_H), GROUND + (255,))
    x = (THUMB_W - ROUNDEL_PX) // 2
    y = (THUMB_H - ROUNDEL_PX) // 2
    thumb.alpha_composite(roundel, (x, y))
    out = REPO / "mod" / "mp_lockstep_1" / "image_00.tga"
    thumb.convert("RGB").save(out, format="TGA")     # Pillow writes uncompressed TGA
    check = Image.open(out)
    assert check.size == (THUMB_W, THUMB_H) and check.mode == "RGB", check
    print(f"wrote {out.relative_to(REPO)}  {check.size} {check.mode} uncompressed")

    # a PNG twin of the thumbnail so it can be looked at without a TGA viewer
    thumb.convert("RGB").save(logo_dir / "image_00_preview.png")


if __name__ == "__main__":
    main()
