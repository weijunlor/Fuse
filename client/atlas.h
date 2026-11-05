/** @file client/atlas.h
 * @brief Shared 50-pixel atlas layout, extended for directional art and portraits.
 * @details Zhikai's art integration adds walk rows 6..9, portraits in row 10
 * and player rings in row 11. Existing sprite IDs remain compatible.
 *
 * Texture atlas: every sprite is a 50 x 50 tile in assets/atlas.png. The atlas
 * is the size of the window (800 x 600 = 16 x 12 tiles), so one tile is one
 * cell of the floor grid and the size of a player's hitbox (SQ_WIDTH).
 * Sprite IDs name the tiles, so drawing code never uses pixel coordinates, and
 * the final art can replace atlas.png without code changes as long as the
 * layout below stays the same (see assets/pack_ninja_atlas.py).
 */

#ifndef CLIENT_ATLAS_H
#define CLIENT_ATLAS_H

#include <GLFW/glfw3.h>

/** @brief Atlas image, relative to the directory the client is started from. */
#define ATLAS_PATH "assets/atlas.png"

/** @brief Tile size in pixels (drawn 1:1 on screen). */
#define ATLAS_TILE 50
/** @brief Tiles per atlas row. */
#define ATLAS_COLS 16
/** @brief Tiles per atlas column. */
#define ATLAS_ROWS 12

/** @brief Sprite ID of the tile at (row, col). */
#define SPRITE(row, col) ((row) * ATLAS_COLS + (col))

/** @brief Sprite IDs (tile indices in the atlas).
 * @details Animations are consecutive tiles in a row (step 1), except the
 * player idle animation, whose frames are one row apart (step ATLAS_COLS) so
 * that each player colour is one column.
 */
enum
{
    SPR_PLAYER        = SPRITE(0, 0),  ///< + player index (0-4); frame 1 is the next row
    SPR_BOMB          = SPRITE(2, 0),  ///< 4 frames: pulse, last is the brightest
    SPR_SPARK         = SPRITE(2, 4),  ///< 2 frames
    SPR_EXPLOSION     = SPRITE(3, 0),  ///< 6 frames: fireball grows, then smoke fades
    SPR_EMBER         = SPRITE(4, 0),  ///< 2 frames: flicker
    SPR_PICKUP_SPEED  = SPRITE(4, 2),  ///< Speed pickup
    SPR_PICKUP_SHIELD = SPRITE(4, 3),  ///< Shield pickup
    SPR_PICKUP_FUSE   = SPRITE(4, 4),  ///< Fuse-time pickup
    SPR_SHIELD_BUBBLE = SPRITE(4, 5),  ///< Bubble drawn over a shielded player
    SPR_FLOOR         = SPRITE(5, 0),  ///< Floor tile
    SPR_WALL          = SPRITE(5, 1),  ///< Wall tile
    /// Four rows: down, up, left, right. Each player occupies three columns.
    SPR_PLAYER_WALK   = SPRITE(6, 0),
    SPR_PORTRAIT      = SPRITE(10, 0), ///< + player index
    SPR_PLAYER_RING   = SPRITE(11, 0)  ///< + player index
};

/** @name Animation frame counts and sprite-ID steps
 * @{ */
#define PLAYER_FRAMES    2
#define PLAYER_STEP      ATLAS_COLS
#define BOMB_FRAMES      4
#define SPARK_FRAMES     2
#define EXPLOSION_FRAMES 6
#define EMBER_FRAMES     2
#define WALK_FRAMES      3
#define SHIELD_FRAMES    6
/** @} */

/** @brief Load assets/atlas.png into an OpenGL texture.
 * @details Nearest-neighbour filtering keeps the pixel art crisp.
 * @pre A current GL context; the client started from the repo root.
 * @return The texture name. Exits the process if the file is missing or has
 *         the wrong size.
 */
GLuint atlas_load(void);

/** @brief Texture coordinates (0..1) of a sprite's tile.
 * @param a_int_sprite Sprite ID.
 * @param[out] u0 Left edge.
 * @param[out] v0 Top edge.
 * @param[out] u1 Right edge.
 * @param[out] v1 Bottom edge.
 */
void atlas_uv(int a_int_sprite, float *u0, float *v0, float *u1, float *v1);

/** @brief Pixel position in the atlas image of a sprite's tile (top-left corner).
 * @param a_int_sprite Sprite ID.
 * @param[out] x Texel column.
 * @param[out] y Texel row.
 */
void atlas_texel(int a_int_sprite, int *x, int *y);

/** @brief Sprite ID of the current frame of a looping animation.
 * @param a_int_first Sprite ID of frame 0.
 * @param a_int_frames Number of frames.
 * @param a_int_step Sprite-ID step between frames (1 or ATLAS_COLS).
 * @param a_d_time_s Current time, seconds.
 * @param a_d_frame_s Seconds each frame is shown.
 * @return Sprite ID to draw.
 */
int atlas_frame(int a_int_first, int a_int_frames, int a_int_step,
                double a_d_time_s, double a_d_frame_s);

#endif /* CLIENT_ATLAS_H */
