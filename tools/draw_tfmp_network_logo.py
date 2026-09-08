#!/usr/bin/env python3
"""Draw a TFMP Ethernet roundel entirely from geometric shapes.

No input images, image models, or font files are used.

Install:  python -m pip install Pillow
Run:      python draw_tfmp_network_logo.py
Options:  python draw_tfmp_network_logo.py --blue '#007BFF' --size 2048 --transparent

Creates matching PNG and SVG files. All lettering is custom geometric paths.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
from dataclasses import dataclass

# All geometry uses a 1000 x 1000 coordinate system.
BLACK = '#000000'
WHITE = '#FFFFFF'


def polygon(*points: tuple[float, float]) -> list[tuple]:
    return [('M', *points[0]), *[('L', *point) for point in points[1:]], ('Z',)]


TOP = polygon((190, 324), (456, 293), (500, 268), (544, 293), (810, 324))
BOTTOM = polygon((190, 676), (810, 676), (544, 707), (500, 732), (456, 707))
T = polygon((148, 380), (310, 380), (310, 432), (255, 432),
            (255, 620), (203, 620), (203, 432), (148, 432))
F = polygon((323, 380), (463, 380), (463, 432), (375, 432),
            (375, 474), (450, 474), (450, 524), (375, 524),
            (375, 620), (323, 620))
M = polygon((477, 620), (477, 380), (525, 380), (574, 465),
            (624, 380), (672, 380), (672, 620), (621, 620),
            (621, 480), (574, 556), (528, 480), (528, 620))
P = [('M', 686, 380), ('L', 782, 380),
     ('C', 827, 380, 852, 405, 852, 452),
     ('C', 852, 498, 827, 523, 782, 523),
     ('L', 738, 523), ('L', 738, 620), ('L', 686, 620), ('Z',)]
P_COUNTER = [('M', 738, 432), ('L', 778, 432),
             ('C', 793, 432, 801, 439, 801, 452),
             ('C', 801, 465, 793, 472, 778, 472),
             ('L', 738, 472), ('Z',)]


@dataclass(frozen=True)
class Shape:
    commands: list[tuple]
    fill: str | None = None
    stroke: str | None = None
    width: float = 0


def rounded_rect(x: float, y: float, w: float, h: float,
                 r: float = 0) -> list[tuple]:
    """A rounded rectangle expressed using cubic Bezier curves."""
    r = max(0.0, min(r, w / 2, h / 2))
    k = 0.5522847498307936
    return [('M', x+r, y), ('L', x+w-r, y),
            ('C', x+w-r+k*r, y, x+w, y+r-k*r, x+w, y+r),
            ('L', x+w, y+h-r),
            ('C', x+w, y+h-r+k*r, x+w-r+k*r, y+h, x+w-r, y+h),
            ('L', x+r, y+h),
            ('C', x+r-k*r, y+h, x, y+h-r+k*r, x, y+h-r),
            ('L', x, y+r),
            ('C', x, y+r-k*r, x+r-k*r, y, x+r, y), ('Z',)]


def ethernet_port(cx: float, cy: float, blue: str) -> list[Shape]:
    """Small front-facing socket with a stepped latch recess and eight contacts."""
    shapes = [
        Shape(rounded_rect(cx-43, cy-34, 86, 68, 7), BLACK, WHITE, 4),
        Shape(polygon((cx-29, cy-20), (cx+29, cy-20),
                      (cx+29, cy+10), (cx+15, cy+10),
                      (cx+15, cy+21), (cx-15, cy+21),
                      (cx-15, cy+10), (cx-29, cy+10)),
              BLACK, blue, 3.5),
    ]
    # Eight individually drawn contacts, rather than a font or imported icon.
    for i in range(8):
        x = cx - 23 + i * 6.15
        shapes.append(Shape(rounded_rect(x, cy-17, 3, 11, 0.8), WHITE))
    # Tiny status lights in the lower bezel.
    shapes.extend([
        Shape(rounded_rect(cx-34, cy+21, 8, 4, 1.5), blue),
        Shape(rounded_rect(cx+26, cy+21, 8, 4, 1.5), WHITE),
    ])
    return shapes


def reflected(commands: list[tuple]) -> list[tuple]:
    """Reflect a path in the vertical centerline of the badge."""
    result = []
    for op, *values in commands:
        result.append(tuple([op] + [1000-v if i % 2 == 0 else v
                                   for i, v in enumerate(values)]))
    return result


def ethernet_plugs(blue: str) -> list[Shape]:
    """A mirrored pair of modular cable ends, pointing toward each other."""
    left = [
        # Blue strain relief where the round cable meets the connector.
        Shape(rounded_rect(388, 790, 25, 33, 5), blue),
        # Clear-shell silhouette represented in white/black.
        Shape(polygon((410, 785), (450, 785), (465, 794),
                      (474, 794), (474, 820), (465, 828),
                      (410, 828)), BLACK, WHITE, 3.5),
        # The connector's characteristic retention clip.
        Shape(polygon((417, 785), (423, 777), (446, 777),
                      (455, 785)), blue),
        Shape([('M', 396, 794), ('L', 396, 819)], None, BLACK, 2),
        Shape([('M', 403, 794), ('L', 403, 819)], None, BLACK, 2),
        Shape([('M', 418, 809), ('L', 446, 809)], None, blue, 3),
    ]
    # Contact comb at the nose; pins are geometric rectangles.
    for i in range(8):
        left.append(Shape(rounded_rect(451 + i*2.55, 796, 1.6, 11, 0.4), WHITE))
    right = [Shape(reflected(s.commands), s.fill, s.stroke, s.width) for s in left]
    return left + right


def scene(blue: str):
    """Shared geometry for the raster and true-vector exports."""
    circles = [(460, BLACK), (432, WHITE), (422, blue),
               (390, WHITE), (379, BLACK)]
    paths = [Shape(TOP, blue), Shape(BOTTOM, blue), Shape(T, WHITE),
             Shape(F, WHITE), Shape(M, blue), Shape(P, WHITE),
             Shape(P_COUNTER, BLACK)]

    # Fine connecting cable under the pair of top sockets.
    paths.append(Shape([
        ('M', 401, 232), ('L', 401, 240),
        ('C', 401, 252, 410, 257, 424, 257),
        ('L', 576, 257),
        ('C', 590, 257, 599, 252, 599, 240), ('L', 599, 232),
    ], None, blue, 5))
    paths.extend(ethernet_port(401, 207, blue))
    paths.extend(ethernet_port(599, 207, blue))

    # A single smooth patch cable links the two inward-facing modular plugs.
    lower = [Shape([
        ('M', 389, 806),
        ('C', 320, 806, 337, 854, 414, 854),
        ('L', 586, 854),
        ('C', 663, 854, 680, 806, 611, 806),
    ], None, blue, 8)]
    lower.extend(ethernet_plugs(blue))
    # Lift the cable group slightly for equal breathing room above and below it.
    for shape in lower:
        commands = [tuple([op] + [v - 12 if i % 2 else v
                                  for i, v in enumerate(values)])
                    for op, *values in shape.commands]
        paths.append(Shape(commands, shape.fill, shape.stroke, shape.width))
    return circles, paths


def flatten_path(commands: list[tuple], steps: int = 64) -> list[tuple]:
    """Sample cubic Bezier curves for Pillow's polygon renderer."""
    points = []
    current = (0.0, 0.0)
    for command in commands:
        operation, *values = command
        if operation in ('M', 'L'):
            current = (values[0], values[1])
            points.append(current)
        elif operation == 'C':
            x0, y0 = current
            x1, y1, x2, y2, x3, y3 = values
            for step in range(1, steps + 1):
                t = step / steps
                u = 1 - t
                points.append((u**3*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x3,
                               u**3*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y3))
            current = (x3, y3)
        elif operation == 'Z':
            if points and points[-1] != points[0]:
                points.append(points[0])
        else:
            raise ValueError(f'Unsupported path command: {operation}')
    return points


def draw_logo(output: Path | str = 'tfmp_network_blue', size: int = 2048,
              blue: str = '#007BFF', transparent: bool = False) -> tuple[Path, Path]:
    """Save a supersampled PNG and a true vector SVG; return their paths."""
    try:
        from PIL import Image, ImageDraw
    except ImportError as error:
        raise RuntimeError('Install Pillow: python -m pip install Pillow') from error

    if not isinstance(size, int) or isinstance(size, bool) or not 64 <= size <= 4096:
        raise ValueError('Size must be an integer from 64 to 4096.')
    if not re.fullmatch(r'#[0-9a-fA-F]{6}', blue):
        raise ValueError('Blue must be a six-digit hex color, such as #007BFF.')

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    png_path = Path(str(output) + '.png')
    svg_path = Path(str(output) + '.svg')
    circles, paths = scene(blue)

    # Supersampling gives the raster image smooth edges at any output size.
    supersample = 4 if size <= 2048 else 2
    scale = size * supersample / 1000
    background = (0, 0, 0, 0) if transparent else WHITE
    image = Image.new('RGBA', (size * supersample, size * supersample), background)
    draw = ImageDraw.Draw(image)
    for radius, color in circles:
        draw.ellipse(tuple(value * scale for value in
                           (500-radius, 500-radius, 500+radius, 500+radius)), fill=color)
    for shape in paths:
        points = [(x * scale, y * scale) for x, y in flatten_path(shape.commands)]
        if shape.fill:
            draw.polygon(points, fill=shape.fill)
        if shape.stroke and shape.width > 0:
            width = max(1, round(shape.width * scale))
            draw.line(points, fill=shape.stroke, width=width, joint='curve')
            # Match SVG round linecaps on open curves.
            if shape.commands[-1][0] != 'Z':
                radius = width / 2
                for x, y in (points[0], points[-1]):
                    draw.ellipse((x-radius, y-radius, x+radius, y+radius),
                                 fill=shape.stroke)
    image = image.resize((size, size), Image.Resampling.LANCZOS)
    if not transparent:
        image = image.convert('RGB')
    image.save(png_path, dpi=(300, 300))

    # SVG preserves the original curves and does not contain an embedded bitmap.
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000" '
           'viewBox="0 0 1000 1000" role="img" aria-labelledby="title desc">',
           '  <title id="title">TFMP Ethernet roundel</title>',
           '  <desc id="desc">Blue, black and white circular TFMP logo with two Ethernet sockets and a patch cable.</desc>']
    if not transparent:
        svg.append(f'  <rect width="1000" height="1000" fill="{WHITE}"/>')
    for radius, color in circles:
        svg.append(f'  <circle cx="500" cy="500" r="{radius}" fill="{color}"/>')
    for shape in paths:
        data = ' '.join(str(value) for command in shape.commands for value in command)
        attributes = f'fill="{shape.fill or "none"}"'
        if shape.stroke:
            attributes += (f' stroke="{shape.stroke}" stroke-width="{shape.width}"'
                           ' stroke-linecap="round" stroke-linejoin="round"')
        svg.append(f'  <path d="{data}" {attributes}/>')
    svg.append('</svg>')
    svg_path.write_text('\n'.join(svg) + '\n', encoding='utf-8')
    return png_path, svg_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=Path('tfmp_network_blue'),
                        help='Output path prefix, without an extension.')
    parser.add_argument('--size', type=int, default=2048, help='PNG width and height.')
    parser.add_argument('--blue', default='#007BFF', help='Accent color in #RRGGBB form.')
    parser.add_argument('--transparent', action='store_true',
                        help='Make the area outside the roundel transparent.')
    args = parser.parse_args()
    try:
        files = draw_logo(args.output, args.size, args.blue, args.transparent)
    except (ValueError, OSError, RuntimeError) as error:
        parser.exit(1, f'Error: {error}\n')
    for file in files:
        print(f'Created: {file.resolve()}')


if __name__ == '__main__':
    main()
