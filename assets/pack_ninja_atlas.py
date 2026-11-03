#!/usr/bin/env python3
"""Pack the curated Ninja Adventure sources into FUSE's 50px cells.

Requires Pillow (python3-pil). Run from any directory. Source art is kept in
ninja_adventure/; no download or original ZIP is needed to reproduce the atlas.
The old sprite IDs remain valid; rows 6-9 add directional walks, row 10 portraits
and row 11 player rings. See README.md and atlas_manifest.json for the layout.
"""
import json
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "ninja_adventure"
TILE, COLS, ROWS = 50, 16, 12
CHARACTERS = ("NinjaRed", "GladiatorBlue", "MaskFrog", "KnightGold", "SorcererBlack")
COLOURS = ("#ed526b", "#57b8ee", "#8acb5b", "#f3ca61", "#bc8cdd")
NEAREST = Image.Resampling.NEAREST


def load(name):
    with Image.open(SOURCE / name) as image:
        return image.convert("RGBA")


def cell(image, scale=None):
    """Integer scaling, centred on a transparent 50px cell; never stretch art."""
    if scale is None:
        scale = max(1, TILE // max(image.size))
    image = image.resize((image.width * scale, image.height * scale), NEAREST)
    if image.width > TILE or image.height > TILE:
        raise ValueError(f"Sprite does not fit a cell: {image.size}")
    output = Image.new("RGBA", (TILE, TILE))
    output.alpha_composite(image, ((TILE - image.width) // 2, (TILE - image.height) // 2))
    return output


def badge(kind):
    """Custom shield and clock icons; the boot inset comes from the pack."""
    image = Image.new("RGBA", (24, 24))
    draw = ImageDraw.Draw(image)
    colour = {"speed": "#d78540", "shield": "#398fba", "fuse": "#6c9853"}[kind]
    draw.rounded_rectangle((1, 1, 22, 22), radius=4, fill="#283844", outline="#f4dda4", width=1)
    draw.rounded_rectangle((3, 3, 20, 20), radius=3, fill=colour)
    if kind == "speed":
        boot = load("Ui/Skill Icon/Items & Weapon/Boot.png").crop((4, 4, 20, 20))
        # Remove the flat orange icon background, preserving the boot pixels.
        pixels = boot.load()
        for y in range(boot.height):
            for x in range(boot.width):
                r, g, b, a = pixels[x, y]
                if r > 180 and r > g * 1.25 and g > b * 1.3:
                    pixels[x, y] = (r, g, b, 0)
        image.alpha_composite(boot, (4, 4))
    elif kind == "shield":
        draw.polygon(((7, 6), (12, 5), (17, 6), (17, 12), (15, 16), (12, 19), (9, 16), (7, 12)),
                     fill="#e2f8ee", outline="#223f56")
        draw.line((12, 7, 12, 16), fill="#73cbd3", width=2)
    else:
        draw.ellipse((6, 6, 18, 18), fill="#f8edc2", outline="#293d3e", width=1)
        draw.rectangle((10, 4, 14, 5), fill="#f8edc2")
        draw.line((12, 8, 12, 12, 15, 12), fill="#365046", width=2)
    return cell(image, 2)


def main():
    atlas = Image.new("RGBA", (COLS * TILE, ROWS * TILE))
    entries = []

    def put(row, col, image, name, source):
        if image.size != (TILE, TILE) or not image.getbbox():
            raise ValueError(f"Empty or incorrectly sized sprite: {name}")
        if any(entry["row"] == row and entry["col"] == col for entry in entries):
            raise ValueError(f"Duplicate cell: {row}, {col}")
        atlas.alpha_composite(image, (col * TILE, row * TILE))
        entries.append(dict(name=name, row=row, col=col, sprite=row * COLS + col, source=source))

    for player, character in enumerate(CHARACTERS):
        source = f"Actor/Character/{character}/SpriteSheet.png"
        sheet = load(source)
        # Original sheets: four directions in columns; idle and two walk poses
        # in the first three 16px rows. Later rows contain attacks/death.
        for frame in range(2):
            art = sheet.crop((0, frame * 16, 16, (frame + 1) * 16))
            put(frame, player, cell(art, 3), f"player-{player + 1}-idle-{frame}", source)
        for direction in range(4):
            for frame in range(3):
                art = sheet.crop((direction * 16, frame * 16, (direction + 1) * 16, (frame + 1) * 16))
                put(6 + direction, player * 3 + frame, cell(art, 3),
                    f"player-{player + 1}-walk-{direction}-{frame}", source)
        portrait = f"Actor/Character/{character}/Faceset.png"
        put(10, player, cell(load(portrait), 1), f"player-{player + 1}-portrait", portrait)
        ring = Image.new("RGBA", (25, 25))
        draw = ImageDraw.Draw(ring)
        draw.ellipse((1, 17, 23, 24), fill=(15, 22, 27, 135), outline=COLOURS[player], width=1)
        put(11, player, cell(ring, 2), f"player-{player + 1}-ring", "FUSE custom player accent")

    bomb_source = "Items/Projectile/Bomb.png"
    bomb = load(bomb_source)
    for frame in range(4):
        art = Image.new("RGBA", (16, 16))
        tinted = bomb.copy()
        # Keep the silhouette; later frames warm the body as the fuse flashes.
        tint = Image.new("RGBA", bomb.size, (240, 76, 54, 255))
        tint.putalpha(bomb.getchannel("A"))
        tinted = Image.blend(tinted, tint, frame * 0.11)
        art.alpha_composite(tinted, (2, 2))
        draw = ImageDraw.Draw(art)
        sx, sy = ((12, 1), (11, 0), (12, 2), (13, 1))[frame]
        draw.line((sx - 1, sy, sx + 1, sy), fill="#fff2ab", width=1)
        draw.line((sx, max(0, sy - 1), sx, sy + 1), fill="#ffb94b", width=1)
        put(2, frame, cell(art, 3), f"bomb-{frame}", bomb_source + " + custom fuse/glow")

    spark_source = "FX/Particle/Spark.png"
    sparks = load(spark_source)
    # Seven 10x8 frames; choose two visibly different bright phases.
    for out, frame in enumerate((2, 3)):
        put(2, 4 + out, cell(sparks.crop((frame * 10, 0, frame * 10 + 10, 8)), 2),
            f"spark-{out}", spark_source)

    explosion_source = "FX/Elemental/Explosion/SpriteSheet.png"
    explosions = load(explosion_source)
    # Nine 40px frames resampled in time to the game's six-frame event.
    for out, frame in enumerate((0, 2, 3, 4, 6, 8)):
        put(3, out, cell(explosions.crop((frame * 40, 0, (frame + 1) * 40, 40)), 1),
            f"explosion-{out}", explosion_source)

    fire_source = "FX/Particle/Fire.png"
    fire = load(fire_source)
    # Twelve 8x12 frames; late phases are small, fading floor embers.
    for out, frame in enumerate((8, 9)):
        put(4, out, cell(fire.crop((frame * 8, 0, (frame + 1) * 8, 12)), 2),
            f"ember-{out}", fire_source)
    for col, kind in enumerate(("speed", "shield", "fuse"), 2):
        put(4, col, badge(kind), f"pickup-{kind}",
            "Ui/Skill Icon/Items & Weapon/Boot.png + custom badge" if kind == "speed" else "FUSE custom icon")

    # The pack's shield is a sweeping arc. Keep a complete protective outline
    # and animate the arc inside it, rather than making protection invisible
    # during the transparent phases of the source animation.
    shield_source = "FX/Magic/Shield/SpriteSheetBlue.png"
    shield = load(shield_source)
    for frame in range(6):
        art = Image.new("RGBA", (25, 25))
        draw = ImageDraw.Draw(art)
        draw.ellipse((1, 1, 23, 23), outline=(146, 230, 231, 185), width=1)
        art.alpha_composite(shield.crop((frame * 24, 0, (frame + 1) * 24, 25)), (0, 0))
        put(4, 5 + frame, cell(art, 2), f"shield-{frame}", shield_source + " + custom outline")

    floor_source = "Backgrounds/Tilesets/TilesetFloor.png"
    floor = load(floor_source).crop((192, 240, 208, 256)).resize((48, 48), NEAREST)
    # Extend the periodic tile by two pixels to fill the 50px cell; sprites
    # retain their integer pixel scale and the arena has no transparent seams.
    ground = Image.new("RGBA", (TILE, TILE))
    for y in (0, 48):
        for x in (0, 48):
            ground.alpha_composite(floor, (x, y))
    if ground.getchannel("A").getextrema() != (255, 255):
        raise ValueError("Selected floor tile is not opaque")
    put(5, 0, ground, "floor", floor_source + " [192,240,208,256]")
    wall_source = "Backgrounds/Tilesets/TilesetDungeon.png"
    wall = load(wall_source).crop((0, 32, 16, 48))
    put(5, 1, cell(wall, 3), "wall", wall_source + " [0,32,16,48]")

    atlas.save(ROOT / "atlas.png")
    manifest = dict(width=800, height=600, cell_size=TILE, characters=list(CHARACTERS), sprites=entries)
    (ROOT / "atlas_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Packed {len(entries)} sprites into {ROOT / 'atlas.png'} (800x600 RGBA)")


if __name__ == "__main__":
    main()
