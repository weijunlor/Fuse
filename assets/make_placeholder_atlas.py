#!/usr/bin/env python3
"""assets/make_placeholder_atlas.py
Generate assets/atlas.png: placeholder pixel art for every sprite in
client/atlas.h, so the client can be built and tested before the final art
exists. The final art replaces atlas.png with the SAME tile layout, and no
code changes are needed.

Layout: 800 x 600 pixels (the window size) = 16 x 12 tiles of 50 x 50, so one
tile is one floor cell and the size of a player's hitbox. Each sprite is drawn
as 25 x 25 art pixels and scaled up 2x (nearest neighbour), so it stays crisp.
Unused tiles are transparent, free for new sprites.

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

TILE = 50          # tile size in the atlas (screen pixels)
ART = 25           # art pixels per tile side
SCALE = TILE // ART
COLS, ROWS = 16, 12  # tiles per atlas row / column (800 x 600)

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
    """Rounded body with outline and eyes; the body fills 21 x 20 of the
    25 x 25 art pixels, close to the whole 50 px hitbox."""
    img = new_tile()
    top = 2 + bob
    rect(img, 2, top, 22, top + 19, rgba(darker(colour)))        # outline
    rect(img, 3, top + 1, 21, top + 18, rgba(colour))            # body
    rect(img, 3, top + 1, 21, top + 3, rgba(tuple(min(255, v + 50) for v in colour)))  # highlight
    for x, y in [(2, top), (3, top), (2, top + 1), (22, top), (21, top), (22, top + 1),
                 (2, top + 19), (3, top + 19), (2, top + 18),
                 (22, top + 19), (21, top + 19), (22, top + 18)]:
        img.putpixel((x, y), CLEAR)                              # rounded corners
    for ex in (7, 14):                                           # eyes
        rect(img, ex, top + 7, ex + 3, top + 10, WHITE)
        rect(img, ex + 2, top + 9, ex + 3, top + 10, BLACK)
    rect(img, 10, top + 14, 14, top + 14, rgba(darker(colour, 0.4)))  # mouth
    return img


def bomb(frame):
    """Black bomb with a fuse; glows redder and swells on later frames."""
    img = new_tile()
    r = 8 + (1 if frame >= 2 else 0)
    glow = [(0, 0, 0), (60, 10, 10), (130, 20, 20), (200, 30, 30)][frame]
    if frame > 0:
        disc(img, 12, 14, r + 2, rgba(glow, 120))
    disc(img, 12, 14, r, BLACK)
    rect(img, 8, 10, 9, 11, (110, 110, 120, 255))                # highlight
    rect(img, 11, 3, 13, 5, (90, 90, 100, 255))                  # cap
    rect(img, 12, 1, 12, 2, (150, 110, 60, 255))                 # fuse
    rect(img, 13, 0, 14, 1, (255, 200, 60, 255))                 # lit end
    return img


def spark(frame):
    """Small star that flips between + and x shapes."""
    img = new_tile()
    c = (255, 220, 80, 255) if frame == 0 else (255, 140, 40, 255)
    rect(img, 11, 11, 13, 13, c)
    if frame == 0:
        dirs = [(0, -1), (0, 1), (-1, 0), (1, 0)]
    else:
        dirs = [(-1, -1), (1, 1), (-1, 1), (1, -1)]
    for dx, dy in dirs:
        for k in range(2, 6):
            img.putpixel((12 + dx * k, 12 + dy * k), c)
    return img


def explosion(frame):
    """Frames 0-3: growing fireball; frames 4-5: fading smoke ring."""
    img = new_tile()
    if frame < 4:
        r = 3 + frame * 3
        disc(img, 12, 12, r, (255, 120, 30, 255))
        disc(img, 12, 12, max(1, r - 3), (255, 220, 90, 255))
    else:
        a = 160 if frame == 4 else 70
        ring(img, 12, 12, 8, 11, (120, 120, 120, a))
    return img


def ember(frame):
    """A few 2 x 2 glowing specks that flicker."""
    img = new_tile()
    pts = [(8, 15), (14, 13), (12, 19), (17, 17), (10, 10)]
    for i, (x, y) in enumerate(pts):
        bright = (i + frame) % 2 == 0
        rect(img, x, y, x + 1, y + 1, (255, 160, 40, 255) if bright else (200, 60, 20, 255))
    return img


def pickup(kind):
    """Round token: speed (bolt), shield (shield), fuse reset (plus)."""
    img = new_tile()
    base = {"speed": (40, 90, 200), "shield": (40, 170, 190), "fuse": (60, 160, 70)}[kind]
    disc(img, 12, 12, 9, rgba(darker(base)))
    disc(img, 12, 12, 8, rgba(base))
    yellow = (255, 230, 60, 255)
    if kind == "speed":
        for y, (x0, x1) in zip(range(6, 19), [(13, 14), (12, 13), (11, 12), (10, 11), (9, 10),
                                              (9, 15), (9, 15), (13, 14), (12, 13), (11, 12),
                                              (10, 11), (9, 10), (9, 9)]):
            rect(img, x0, y, x1, y, yellow)
    elif kind == "shield":
        rect(img, 8, 7, 16, 12, WHITE)
        rect(img, 9, 13, 15, 14, WHITE)
        rect(img, 10, 15, 14, 15, WHITE)
        rect(img, 11, 16, 13, 16, WHITE)
        rect(img, 12, 17, 12, 17, WHITE)
    else:
        rect(img, 11, 6, 13, 18, WHITE)
        rect(img, 6, 11, 18, 13, WHITE)
    return img


def shield_bubble():
    img = new_tile()
    ring(img, 12, 12, 10.6, 12.1, (120, 230, 255, 170))
    return img


def floor():
    img = Image.new("RGBA", (ART, ART), (205, 200, 185, 255))
    for p in [(4, 6), (17, 3), (11, 19), (22, 14), (2, 22), (14, 10), (20, 22)]:
        img.putpixel(p, (185, 178, 162, 255))
    return img


def wall():
    img = Image.new("RGBA", (ART, ART), (120, 70, 55, 255))
    mortar = (80, 45, 35, 255)
    for y in (0, 8, 16, 24):
        rect(img, 0, y, ART - 1, y, mortar)                      # mortar rows
    for y0, xs in ((1, (0, 12)), (9, (6, 18)), (17, (0, 12))):
        for x in xs:
            rect(img, x, y0, x, y0 + 6, mortar)                  # mortar joints
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

    atlas = Image.new("RGBA", (COLS * TILE, ROWS * TILE), CLEAR)
    for (row, col), art in tiles.items():
        big = art.resize((TILE, TILE), Image.NEAREST)
        atlas.paste(big, (col * TILE, row * TILE))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "atlas.png")
    atlas.save(out)
    print(f"wrote {out} ({len(tiles)} sprites)")


if __name__ == "__main__":
    main()
