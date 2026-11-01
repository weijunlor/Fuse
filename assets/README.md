# FUSE sprite atlas

`atlas.png` is the runtime texture: **800 x 600 RGBA**, with **16 x 12 cells
of 50 x 50 pixels**. The client loads it automatically; the original atlas is
preserved as `atlas-placeholder.png` for reference.

The selected source PNGs live in `ninja_adventure/`. They come from
[Ninja Adventure by Pixel-Boy and AAA](https://pixel-boy.itch.io/ninja-adventure-asset-pack),
from the supplied `Ninja Adventure - Asset Pack.zip`. The included
`ninja_adventure/LICENSE.txt` is the pack's CC0 licence. The boot badge and bomb
are adapted from the pack; the clock, shield icon, player rings and complete
shield outlines are custom FUSE additions.

Regenerate from the checked-in sources (no ZIP or network needed):

```bash
# Requires Pillow; on Ubuntu: sudo apt install python3-pil
python3 assets/pack_ninja_atlas.py
make -j4 build/client
```

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
