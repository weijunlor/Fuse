# FUSE sprite atlas

`atlas.png` is the runtime texture: **800 x 600 RGBA**, with **16 x 12 cells
of 50 x 50 pixels**. The client loads it automatically. `atlas.png` is the only
copy of the art in the repository: the earlier placeholder atlas
(`atlas-placeholder.png`, made by `make_placeholder_atlas.py`) has been removed.

## Credits and licence

The character, effect and item art comes from
[Ninja Adventure by Pixel-Boy and AAA](https://pixel-boy.itch.io/ninja-adventure-asset-pack),
released under **CC0 1.0** (public domain dedication: no attribution required,
credited here anyway). The boot badge and bomb are adapted from the pack; the
clock, shield icon, player rings and complete shield outlines are custom FUSE
additions.

The source PNGs and the pack's `LICENSE.txt` are no longer kept in the
repository (removed in commit c7cd424 to keep the project small).

## Regenerating the atlas

Only needed when changing the art; the game uses the checked-in `atlas.png`.
`pack_ninja_atlas.py` reads its sources from `assets/ninja_adventure/`, so put
them back first, by either:

- restoring the removed files from git:
  `git checkout c7cd424^ -- assets/ninja_adventure`, or
- extracting the files listed under `"source"` in `atlas_manifest.json` from the
  original `Ninja Adventure - Asset Pack.zip` into `assets/ninja_adventure/`,
  keeping the pack's folder structure.

```bash
# Requires Pillow; on Ubuntu: sudo apt install python3-pil
python3 assets/pack_ninja_atlas.py
make -j4 build/client
```

Do not commit `assets/ninja_adventure/` again unless the team decides to.

Most sprites use integer nearest-neighbour scaling, with transparent padding.
16px characters become 48px art inside a 50px cell. Effect frames retain a
consistent scale and position. Floor tiles extend their repeated pattern over
the two remaining pixels so the arena has no transparent seams.

| Row | Cells | Contents |
| --- | --- | --- |
| 0-1 | 0-4 | Two idle poses per player (legacy layout) |
| 2 | 0-3, 4-5 | Four bomb/fuse phases, two sparks |
| 3 | 0-5 | Six explosion phases |
| 4 | 0-1, 2-4, 5-10 | Embers, speed/shield/fuse-reset pickups, six shield phases |
| 5 | 0-1 | Floor and wall |
| 6-9 | 0-14 | Down/up/left/right walking, three cells per player |
| 10 | 0-4 | Lobby portraits |
| 11 | 0-4 | Player identification rings |

Player IDs select NinjaRed, GladiatorBlue, MaskFrog, KnightGold and SorcererBlack
in that order, with red/blue/green/gold/purple rings. These are visual choices;
characters have the same rules and 50px hitboxes. Walking follows snapshot
velocity, and idle characters retain their last direction. Bomb flashing speeds
up below five and two seconds; the server remains responsible for detonation.

`atlas_manifest.json` lists every occupied cell and its source. The generator
checks cell size, duplicate placement, nonempty sprites and an opaque floor.
