/* client/atlas.h
Texture atlas: every sprite is a 64 x 64 tile in assets/atlas.png (8 x 8 tiles).
Sprite IDs name the tiles, so drawing code never uses pixel coordinates, and
the final art can replace atlas.png without code changes as long as the
layout below stays the same (see assets/make_placeholder_atlas.py). */

#ifndef CLIENT_ATLAS_H
#define CLIENT_ATLAS_H

#include <GLFW/glfw3.h>

// Atlas image, relative to the directory the client is started from
#define ATLAS_PATH "assets/atlas.png"

// Tile size in pixels and tiles per atlas side
#define ATLAS_TILE 64
#define ATLAS_GRID 8

// Sprite ID of the tile at (row, col)
#define SPRITE(row, col) ((row) * ATLAS_GRID + (col))

/* Sprite IDs. Animations are consecutive tiles in a row (step 1), except the
player idle animation, whose frames are one row apart (step ATLAS_GRID) so
that each player colour is one column. */
enum
{
    SPR_PLAYER        = SPRITE(0, 0),  // + player index (0-4); frame 1 is the next row
    SPR_BOMB          = SPRITE(2, 0),  // 4 frames: pulse, last is the brightest
    SPR_SPARK         = SPRITE(2, 4),  // 2 frames
    SPR_EXPLOSION     = SPRITE(3, 0),  // 6 frames: fireball grows, then smoke fades
    SPR_EMBER         = SPRITE(4, 0),  // 2 frames: flicker
    SPR_PICKUP_SPEED  = SPRITE(4, 2),
    SPR_PICKUP_SHIELD = SPRITE(4, 3),
    SPR_PICKUP_FUSE   = SPRITE(4, 4),
    SPR_SHIELD_BUBBLE = SPRITE(4, 5),
    SPR_FLOOR         = SPRITE(5, 0),
    SPR_WALL          = SPRITE(5, 1)
};

// Number of animation frames and the sprite-ID step between them
#define PLAYER_FRAMES    2
#define PLAYER_STEP      ATLAS_GRID
#define BOMB_FRAMES      4
#define SPARK_FRAMES     2
#define EXPLOSION_FRAMES 6
#define EMBER_FRAMES     2

// Load assets/atlas.png into an OpenGL texture with nearest-neighbour
// filtering (crisp pixel art). Needs a current GL context. Exits on failure.
GLuint atlas_load(void);

// Texture coordinates (0..1) of a sprite's tile: (u0, v0) top-left, (u1, v1) bottom-right
void atlas_uv(int a_int_sprite, float *u0, float *v0, float *u1, float *v1);

// Sprite ID of the current frame of a looping animation: a_int_frames frames
// starting at a_int_first, a_int_step apart, each shown for a_d_frame_s seconds
int atlas_frame(int a_int_first, int a_int_frames, int a_int_step,
                double a_d_time_s, double a_d_frame_s);

#endif /* CLIENT_ATLAS_H */
