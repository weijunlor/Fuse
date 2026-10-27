#!/usr/bin/env python3
"""assets/make_placeholder_atlas.py
Generate assets/atlas.png: placeholder pixel art for every sprite in
client/atlas.h, so the client can be built and tested before the final art
exists. The final art replaces atlas.png with the SAME tile layout, and no
code changes are needed.

Layout: 512 x 512 pixels = 8 x 8 tiles of 64 x 64. Each sprite is drawn as
16 x 16 art pixels and scaled up 4x (nearest neighbour), so it stays crisp.

  row 0  player 1-5, idle frame 0      (cols 0-4)
  row 1  player 1-5, idle frame 1      (cols 0-4, body one art pixel lower)
  row 2  bomb pulse frames 0-3         (cols 0-3), spark frames 0-1 (cols 4-5)
  row 3  explosion frames 0-5          (cols 0-5)
  row 4  ember frames 0-1 (cols 0-1), pickup speed/shield/fuse-reset (cols 2-4),
         shield bubble (col 5)
  row 5  floor (col 0), wall (col 1)

Needs Pillow (sudo apt install python3-pil). Run from anywhere:
  python3 assets/make_placeholder_atlas.py
"""

import math
import os

from PIL import Image

TILE = 64          # tile size in the atlas (screen pixels)
ART = 16           # art pixels per tile side
SCALE = TILE // ART
GRID = 8           # tiles per atlas side

# Player body colours, index = player_id - 1 (also used for the HUD later)
PLAYER_COLOURS = [(230, 70, 70), (70, 130, 230), (80, 190, 90),
                  (240, 190, 50), (170, 90, 210)]

CLEAR = (0, 0, 0, 0)
BLACK = (20, 20, 25, 255)
WHITE = (250, 250, 250, 255)


def rgba(c, a=255):
    return (c[0], c[1], c[2], a)


def darker(c, f=0.6):
    return (int(c[0] * f), int(c[1] * f), int(c[2] * f))


def new_tile():
    return Image.new("RGBA", (ART, ART), CLEAR)


def disc(img, cx, cy, r, colour):
    """Filled circle centred on art pixel (cx, cy)."""
    for y in range(ART):
        for x in range(ART):
            # r * r + r instead of r * r: avoids single-pixel spikes at
            # the top, bottom, left and right of small pixel-art circles
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r:
                img.putpixel((x, y), colour)


def ring(img, cx, cy, r_in, r_out, colour):
    for y in range(ART):
        for x in range(ART):
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            if r_in * r_in + r_in <= d2 <= r_out * r_out + r_out:
                img.putpixel((x, y), colour)


def rect(img, x0, y0, x1, y1, colour):
    """Filled rectangle, inclusive corners."""
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            img.putpixel((x, y), colour)


# ---------------------------------------------------------------------------
# Sprites (each returns a 16 x 16 RGBA image)
# ---------------------------------------------------------------------------
def player(colour, bob):
    """Rounded body with outline and eyes; the body fills the 12 x 12 art
    pixels (48 screen pixels) in the middle, close to the 50 px hitbox."""
    img = new_tile()
    top = 2 + bob
    rect(img, 2, top, 13, top + 11, rgba(darker(colour)))        # outline
    rect(img, 3, top + 1, 12, top + 10, rgba(colour))            # body
    rect(img, 3, top + 1, 12, top + 2, rgba(tuple(min(255, v + 50) for v in colour)))  # highlight
    for corner in [(2, top), (13, top), (2, top + 11), (13, top + 11)]:
        img.putpixel(corner, CLEAR)                              # rounded corners
    for ex in (5, 9):                                            # eyes
        rect(img, ex, top + 4, ex + 1, top + 5, WHITE)
        img.putpixel((ex + 1, top + 5), BLACK)
    return img


def bomb(frame):
    """Black bomb with a fuse; glows redder and swells on later frames."""
    img = new_tile()
    r = 5 + (1 if frame >= 2 else 0)
    glow = [(0, 0, 0), (60, 10, 10), (130, 20, 20), (200, 30, 30)][frame]
    if frame > 0:
        disc(img, 8, 9, r + 1, rgba(glow, 120))
    disc(img, 8, 9, r, BLACK)
    img.putpixel((6, 7), (110, 110, 120, 255))                   # highlight
    rect(img, 8, 2, 8, 3, (150, 110, 60, 255))                   # fuse
    img.putpixel((9, 1), (255, 200, 60, 255))                    # lit end
    return img


def spark(frame):
    img = new_tile()
    c = (255, 220, 80, 255) if frame == 0 else (255, 140, 40, 255)
    pts = [(8, 4), (8, 12), (4, 8), (12, 8), (8, 8)] if frame == 0 else \
          [(5, 5), (11, 11), (5, 11), (11, 5), (8, 8)]
    for p in pts:
        img.putpixel(p, c)
    return img


def explosion(frame):
    """Frames 0-3: growing fireball; frames 4-5: fading smoke ring."""
    img = new_tile()
    if frame < 4:
        r = 2 + frame * 2
        disc(img, 8, 8, r, (255, 120, 30, 255))
        disc(img, 8, 8, max(1, r - 2), (255, 220, 90, 255))
    else:
        a = 160 if frame == 4 else 70
        ring(img, 8, 8, 5, 7, (120, 120, 120, a))
    return img


def ember(frame):
    img = new_tile()
    pts = [(6, 10), (9, 9), (8, 12), (11, 11)]
    for i, p in enumerate(pts):
        bright = (i + frame) % 2 == 0
        img.putpixel(p, (255, 160, 40, 255) if bright else (200, 60, 20, 255))
    return img


def pickup(kind):
    """Round token: speed (bolt), shield (shield), fuse reset (plus)."""
    img = new_tile()
    base = {"speed": (40, 90, 200), "shield": (40, 170, 190), "fuse": (60, 160, 70)}[kind]
    disc(img, 8, 8, 6, rgba(darker(base)))
    disc(img, 8, 8, 5, rgba(base))
    if kind == "speed":
        for p in [(9, 4), (8, 5), (7, 6), (7, 7), (8, 7), (9, 7), (8, 8), (7, 9), (6, 10)]:
            img.putpixel(p, (255, 230, 60, 255))
    elif kind == "shield":
        rect(img, 6, 5, 10, 8, WHITE)
        rect(img, 7, 9, 9, 9, WHITE)
        img.putpixel((8, 10), WHITE)
    else:
        rect(img, 7, 5, 8, 10, WHITE)
        rect(img, 5, 7, 10, 8, WHITE)
    return img


def shield_bubble():
    img = new_tile()
    ring(img, 7.5, 7.5, 6.5, 7.6, (120, 230, 255, 170))
    return img


def floor():
    img = Image.new("RGBA", (ART, ART), (205, 200, 185, 255))
    for p in [(3, 4), (11, 2), (7, 12), (14, 9), (1, 14)]:
        img.putpixel(p, (185, 178, 162, 255))
    return img


def wall():
    img = Image.new("RGBA", (ART, ART), (120, 70, 55, 255))
    for y in (0, 5, 10, 15):
        rect(img, 0, y, ART - 1, y, (80, 45, 35, 255))           # mortar rows
    for y0, xs in ((1, (0, 8)), (6, (4, 12)), (11, (0, 8))):
        for x in xs:
            rect(img, x, y0, x, y0 + 3, (80, 45, 35, 255))       # mortar joints
    return img


# ---------------------------------------------------------------------------
# Assemble the atlas
# ---------------------------------------------------------------------------
def main():
    tiles = {}
    for i, c in enumerate(PLAYER_COLOURS):
        tiles[(0, i)] = player(c, 0)
        tiles[(1, i)] = player(c, 1)
    for f in range(4):
        tiles[(2, f)] = bomb(f)
    for f in range(2):
        tiles[(2, 4 + f)] = spark(f)
    for f in range(6):
        tiles[(3, f)] = explosion(f)
    for f in range(2):
        tiles[(4, f)] = ember(f)
    tiles[(4, 2)] = pickup("speed")
    tiles[(4, 3)] = pickup("shield")
    tiles[(4, 4)] = pickup("fuse")
    tiles[(4, 5)] = shield_bubble()
    tiles[(5, 0)] = floor()
    tiles[(5, 1)] = wall()

    atlas = Image.new("RGBA", (GRID * TILE, GRID * TILE), CLEAR)
    for (row, col), art in tiles.items():
        big = art.resize((TILE, TILE), Image.NEAREST)
        atlas.paste(big, (col * TILE, row * TILE))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "atlas.png")
    atlas.save(out)
    print(f"wrote {out} ({len(tiles)} sprites)")


if __name__ == "__main__":
    main()
