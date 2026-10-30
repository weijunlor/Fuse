/* client/render.c
Batched rendering of players, bomb and HUD.

Optimisation #16 (batched drawing). Ways to draw sprites, chosen at start-up
with --render <mode> (see RenderMode in render.h):
- immediate (#16a before): every sprite is its own glBegin/glEnd, i.e.
  4 glTexCoord + 4 glVertex driver calls per sprite.
- float (#16a after): sprites are appended to one vertex array in memory and
  drawn with a single glDrawArrays call per frame, so the driver is called a
  few times per frame instead of ~9 times per sprite.
- short (#16b after, default): as float, but each vertex is 4 shorts (8 bytes)
  instead of 4 floats (16 bytes). Measuring #16a showed that with many sprites
  most of the time is the driver copying the vertex array, so halving its
  size should halve that copy.
Text is still drawn per string (ui.c) in every mode. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLFW/glfw3.h>
#include <stb_image_write.h>   // from libstb-dev; implementation is in libstb.so

#include "render.h"
#include "atlas.h"
#include "net_client.h"
#include "ui.h"
#include "particles.h"

// Seconds each animation frame is shown
#define PLAYER_FRAME_S 0.4
#define STRESS_FRAME_S 0.1
// Seconds per explosion frame; the animation plays once (6 x 0.08 = 0.48 s)
#define EXPLOSION_FRAME_S 0.08

// Floor tiles needed to cover the window (the tile size divides the window
// size exactly, so this is 16 x 12)
#define FLOOR_TILES ((WIDTH / ATLAS_TILE) * (HEIGHT / ATLAS_TILE))

// One corner of a sprite quad. Position and texture coordinates are stored
// together (interleaved), so each corner is one contiguous read for the driver.
// "float" mode: 16 bytes, texture coordinates 0..1
typedef struct
{
    float x, y;
    float u, v;
} QuadVertex;

// "short" mode: 8 bytes. Positions are whole window pixels (the game only uses
// whole pixels); texture coordinates are whole atlas pixels, scaled to 0..1 by
// the texture matrix set in render_init()
typedef struct
{
    short x, y;
    short u, v;
} QuadVertexShort;

static const char *const arr_mode_names[] = { "immediate", "float", "short" };

// Animated test sprite for --stress (stands in for particles and embers)
typedef struct
{
    float x, y;
    int int_first;      // first frame's sprite ID
    int int_frames;     // number of frames
    double d_phase_s;   // animation offset, so the sprites do not blink in sync
} StressSprite;

static GLuint texture_atlas;
static RenderMode render_mode;

// Batch: vertices of every sprite this frame, drawn by batch_flush(). Only
// the array of the current mode is allocated.
static QuadVertex *arr_batch;              // "float" mode
static QuadVertexShort *arr_batch_short;   // "short" mode
static int int_batch_len;   // vertices in use
static int int_batch_cap;   // vertices allocated

static StressSprite *arr_stress;
static int int_num_stress;
static unsigned long arr_u32_seen_explosions[4] = {0};
static unsigned long u32_last_snapshot_tick = 0;
static uint8_t u8_last_snapshot_phase = PHASE_LOBBY;

// Immediate path: one sprite = one glBegin/glEnd
static void sprite_immediate(int a_int_sprite, float x, float y)
{
    float u0, v0, u1, v1;
    atlas_uv(a_int_sprite, &u0, &v0, &u1, &v1);

    glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x, y);
        glTexCoord2f(u0, v1); glVertex2f(x, y + ATLAS_TILE);
        glTexCoord2f(u1, v1); glVertex2f(x + ATLAS_TILE, y + ATLAS_TILE);
        glTexCoord2f(u1, v0); glVertex2f(x + ATLAS_TILE, y);
    glEnd();
}

// Batched path: append the sprite's 4 corners to the batch (no GL call)
static void sprite_batched(int a_int_sprite, float x, float y)
{
    float u0, v0, u1, v1;
    atlas_uv(a_int_sprite, &u0, &v0, &u1, &v1);

    // Capacity is sized in render_init() for everything drawn in a frame
    QuadVertex *p = &arr_batch[int_batch_len];
    p[0] = (QuadVertex){ x,              y,              u0, v0 };
    p[1] = (QuadVertex){ x,              y + ATLAS_TILE, u0, v1 };
    p[2] = (QuadVertex){ x + ATLAS_TILE, y + ATLAS_TILE, u1, v1 };
    p[3] = (QuadVertex){ x + ATLAS_TILE, y,              u1, v0 };
    int_batch_len += 4;
}

// "short" path: as sprite_batched, with 8-byte vertices
static void sprite_batched_short(int a_int_sprite, float x, float y)
{
    int int_u, int_v;
    atlas_texel(a_int_sprite, &int_u, &int_v);
    short sx = (short)x, sy = (short)y;
    short su = (short)int_u, sv = (short)int_v;
    const short T = ATLAS_TILE;

    QuadVertexShort *p = &arr_batch_short[int_batch_len];
    p[0] = (QuadVertexShort){ sx,     sy,     su,     sv     };
    p[1] = (QuadVertexShort){ sx,     sy + T, su,     sv + T };
    p[2] = (QuadVertexShort){ sx + T, sy + T, su + T, sv + T };
    p[3] = (QuadVertexShort){ sx + T, sy,     su + T, sv     };
    int_batch_len += 4;
}

// Draw one sprite with its top-left corner at (x, y), at the atlas tile size
static void render_sprite(int a_int_sprite, float x, float y)
{
    switch (render_mode)
    {
        case RENDER_IMMEDIATE:     sprite_immediate(a_int_sprite, x, y);     break;
        case RENDER_BATCHED_FLOAT: sprite_batched(a_int_sprite, x, y);       break;
        case RENDER_BATCHED_SHORT: sprite_batched_short(a_int_sprite, x, y); break;
    }
}

// Batched paths: draw every queued sprite with one call, then empty the batch
static void batch_flush(void)
{
    if (render_mode == RENDER_IMMEDIATE || int_batch_len == 0)
        return;

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    if (render_mode == RENDER_BATCHED_FLOAT)
    {
        glVertexPointer(2, GL_FLOAT, sizeof(QuadVertex), &arr_batch[0].x);
        glTexCoordPointer(2, GL_FLOAT, sizeof(QuadVertex), &arr_batch[0].u);
    }
    else
    {
        glVertexPointer(2, GL_SHORT, sizeof(QuadVertexShort), &arr_batch_short[0].x);
        glTexCoordPointer(2, GL_SHORT, sizeof(QuadVertexShort), &arr_batch_short[0].u);
    }
    glDrawArrays(GL_QUADS, 0, int_batch_len);
    // Text (ui.c) draws with its own vertex array; a texture-coordinate array
    // left enabled would make the driver read past the end of ours
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    int_batch_len = 0;
}

int render_mode_from_name(const char *a_str_name)
{
    for (int i = 0; i < (int)(sizeof(arr_mode_names) / sizeof(arr_mode_names[0])); i++)
    {
        if (strcmp(a_str_name, arr_mode_names[i]) == 0)
            return i;
    }
    return -1;
}

const char *render_mode_name(RenderMode a_mode)
{
    return arr_mode_names[a_mode];
}

void render_init(RenderMode a_mode, int a_int_stress)
{
    // Needs particles_capacity(), so particles_init() must have run
    /* Use pixel coordinates */
    _Static_assert(ATLAS_TILE == SQ_WIDTH, "player sprites are drawn at hitbox size");
    _Static_assert(WIDTH % ATLAS_TILE == 0 && HEIGHT % ATLAS_TILE == 0,
                   "floor tiles must fill the window exactly");
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);

    texture_atlas = atlas_load();

    // Sprites have transparent pixels around them
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    render_mode = a_mode;

    // "short" mode sends texture coordinates in atlas pixels; the texture
    // matrix scales them to 0..1 on the GPU. Text is drawn untextured, so the
    // matrix can stay set for the whole run.
    if (render_mode == RENDER_BATCHED_SHORT)
    {
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glScalef(1.0f / (ATLAS_TILE * ATLAS_COLS), 1.0f / (ATLAS_TILE * ATLAS_ROWS), 1.0f);
        glMatrixMode(GL_MODELVIEW);
    }

    // Stress sprites at fixed pseudo-random positions (same every run, so
    // runs are comparable): a mix of embers, sparks, bombs and explosions
    static const int arr_kinds[4][2] = {
        { SPR_EMBER, EMBER_FRAMES }, { SPR_SPARK, SPARK_FRAMES },
        { SPR_BOMB, BOMB_FRAMES },   { SPR_EXPLOSION, EXPLOSION_FRAMES } };
    int_num_stress = a_int_stress;
    arr_stress = calloc((size_t)(int_num_stress > 0 ? int_num_stress : 1), sizeof(StressSprite));
    unsigned int u_seed = 12345;
    for (int i = 0; i < int_num_stress; i++)
    {
        // Linear congruential generator (numbers from glibc's rand_r)
        u_seed = u_seed * 1103515245u + 12345u;
        arr_stress[i].x = (float)((u_seed >> 8) % (WIDTH - ATLAS_TILE));
        u_seed = u_seed * 1103515245u + 12345u;
        arr_stress[i].y = (float)((u_seed >> 8) % (HEIGHT - ATLAS_TILE));
        arr_stress[i].int_first = arr_kinds[i % 4][0];
        arr_stress[i].int_frames = arr_kinds[i % 4][1];
        arr_stress[i].d_phase_s = (i % 17) * 0.037;
    }

    // Batch capacity: floor + players + stress sprites + particles, 4 vertices
    // each (particles_init() must run first)
    int_batch_cap = (FLOOR_TILES + MAX_PLAYERS + int_num_stress + particles_capacity()) * 4;
    bool b_alloc_ok = true;
    if (render_mode == RENDER_BATCHED_FLOAT)
        b_alloc_ok = (arr_batch = malloc((size_t)int_batch_cap * sizeof(QuadVertex))) != NULL;
    else if (render_mode == RENDER_BATCHED_SHORT)
        b_alloc_ok = (arr_batch_short = malloc((size_t)int_batch_cap * sizeof(QuadVertexShort))) != NULL;
    if (!b_alloc_ok || arr_stress == NULL)
    {
        fprintf(stderr, "Out of memory for %d sprites\n", int_num_stress);
        exit(1);
    }
    int_batch_len = 0;
}

void render_resize(int a_int_w, int a_int_h)
{
    // The projection (glOrtho in render_init) maps WIDTH x HEIGHT game pixels
    // to the viewport, so only the viewport has to follow the framebuffer.
    // OpenGL's default viewport is the size at context creation, which can be
    // smaller than the final framebuffer: the game then filled only the
    // bottom-left part, leaving a gap at the top and right
    glViewport(0, 0, a_int_w, a_int_h);
}

void render_frame(void)
{
    double d_time_s = glfwGetTime();
    GameSnapshot game_snapshot;
    pthread_mutex_lock(&mutex);
    game_snapshot = latest_snapshot;
    pthread_mutex_unlock(&mutex);

    bool b_new_server = game_snapshot.tick < u32_last_snapshot_tick;
    bool b_new_round = game_snapshot.phase == PHASE_COUNTDOWN &&
                       u8_last_snapshot_phase != PHASE_COUNTDOWN;
    if (b_new_server || b_new_round)
    {
        particles_clear();
        memset(arr_u32_seen_explosions, 0, sizeof arr_u32_seen_explosions);
    }
    if (game_snapshot.tick != 0)
        u32_last_snapshot_tick = game_snapshot.tick;
    u8_last_snapshot_phase = game_snapshot.phase;

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* ----------------------------------------------------- */
    /* Sprites (textured; white colour = original art)       */
    /* ----------------------------------------------------- */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture_atlas);
    glColor3f(1.0f, 1.0f, 1.0f);

    // Floor, tiled over the whole window
    for (int y = 0; y < HEIGHT; y += ATLAS_TILE)
    {
        for (int x = 0; x < WIDTH; x += ATLAS_TILE)
        {
            render_sprite(SPR_FLOOR, x, y);
        }
    }

    // Stress test sprites (--stress N), animated
    for (int i = 0; i < int_num_stress; i++)
    {
        const StressSprite *p = &arr_stress[i];
        int int_sprite = atlas_frame(p->int_first, p->int_frames, 1,
                                     d_time_s + p->d_phase_s, STRESS_FRAME_S);
        render_sprite(int_sprite, p->x, p->y);
    }

    // Particles (explosion sparks and embers), under the players
    for (int i = 0; i < 4; i++)
    {
        const Explosion *p_explosion = &game_snapshot.explosions[i];
        if (p_explosion->tick == 0)
            continue;
        /* Age from server ticks (the explosion's tick and the snapshot's tick
        are both server tick numbers, TICK_HZ per second), so every client
        shows the same frame. The server keeps sending the explosion until the
        next round, but the animation plays once: an explosion older than the
        animation is not drawn, and does not spawn particles in a client
        that only sees it then (e.g. one that joined late). */
        double d_age_s = (double)(game_snapshot.tick - p_explosion->tick) / TICK_HZ;
        bool b_playing = d_age_s < EXPLOSION_FRAMES * EXPLOSION_FRAME_S;
        if (arr_u32_seen_explosions[i] != p_explosion->tick)
        {
            if (b_playing)
                particles_burst((float)p_explosion->x - SQ_WIDTH / 2.0f,
                                (float)p_explosion->y - SQ_WIDTH / 2.0f, 48);
            arr_u32_seen_explosions[i] = p_explosion->tick;
        }
        if (!b_playing)
            continue;
        int int_sprite = atlas_frame(SPR_EXPLOSION, EXPLOSION_FRAMES, 1,
                                     d_age_s, EXPLOSION_FRAME_S);
        render_sprite(int_sprite, p_explosion->x - SQ_WIDTH / 2.0f,
                      p_explosion->y - SQ_WIDTH / 2.0f);
    }
    for (int i = 0; i < 4; i++)
    {
        const Pickup *p_pickup = &game_snapshot.pickups[i];
        int int_sprite = p_pickup->type == PICKUP_SPEED ? SPR_PICKUP_SPEED :
                         p_pickup->type == PICKUP_SHIELD ? SPR_PICKUP_SHIELD :
                         p_pickup->type == PICKUP_FUSE_RESET ? SPR_PICKUP_FUSE : -1;
        if (int_sprite >= 0)
            render_sprite(int_sprite, p_pickup->x, p_pickup->y);
    }
    particles_draw(render_sprite);

    // Players: the sprite is the same size as the SQ_WIDTH hitbox
    for(int i = 0; i < MAX_PLAYERS; i++)
    {
        const PlayerState *p_state = &game_snapshot.players[i];
        if((p_state->flags & PF_ACTIVE) != 0)
        {
            int int_sprite = atlas_frame(SPR_PLAYER + i, PLAYER_FRAMES, PLAYER_STEP,
                                         d_time_s, PLAYER_FRAME_S);
            render_sprite(int_sprite, p_state->x, p_state->y);
            if ((p_state->flags & PF_SHIELD) != 0)
                render_sprite(SPR_SHIELD_BUBBLE, p_state->x, p_state->y);
            if (game_snapshot.bomb_holder == i + 1)
            {
                int int_bomb = atlas_frame(SPR_BOMB, BOMB_FRAMES, 1,
                                            d_time_s, 0.12);
                render_sprite(int_bomb, p_state->x, p_state->y - SQ_WIDTH);
            }
        }
    }

    // Batched mode: everything above is drawn here, in one call
    batch_flush();

    /* ----------------------------------------------------- */
    /* Text (untextured quads)                               */
    /* ----------------------------------------------------- */
    glDisable(GL_TEXTURE_2D);
    glColor3f(0.15f, 0.15f, 0.2f);

    char str_banner[160] = "Waiting for server ...";
    if (int_player_id != -1)
    {
        const char *str_phase = game_snapshot.phase == PHASE_COUNTDOWN ? "Starting" :
                                game_snapshot.phase == PHASE_PLAYING ? "Playing" :
                                game_snapshot.phase == PHASE_ROUND_OVER ? "Round over" :
                                game_snapshot.phase == PHASE_GAME_OVER ? "Game over" :
                                "Waiting";
        
        // Use %02u and add "/ 10" to the final argument
        snprintf(str_banner, sizeof(str_banner), "Player %d %s %u.%02us",
                 int_player_id, str_phase,
                 (unsigned)(game_snapshot.timer_ms / 1000),
                 (unsigned)((game_snapshot.timer_ms % 1000) / 10)); 
    }
    ui_draw_text(230, 20, 3.0f, str_banner);

    for(int i = 0; i < MAX_PLAYERS; i++)
    {
        const PlayerState *p_state = &game_snapshot.players[i];
        if((p_state->flags & PF_ACTIVE) != 0)
        {
            char str_player[32];
            snprintf(str_player, sizeof(str_player), "%d L:%u", i + 1,
                     (unsigned)p_state->lives);
            ui_draw_text(p_state->x + SQ_WIDTH / 2 - 20, p_state->y - 30,
                         2.0f, str_player);
        }
    }
}

void render_save_screenshot(const char *a_str_path)
{
    // Read the whole viewport (= framebuffer, see render_resize), which is
    // larger than WIDTH x HEIGHT when the display is scaled
    GLint arr_viewport[4];
    glGetIntegerv(GL_VIEWPORT, arr_viewport);
    int int_w = arr_viewport[2];
    int int_h = arr_viewport[3];
    unsigned char *arr_pixels = malloc((size_t)int_w * int_h * 3);
    if (arr_pixels == NULL)
    {
        fprintf(stderr, "Out of memory for screenshot\n");
        return;
    }

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, int_w, int_h, GL_RGB, GL_UNSIGNED_BYTE, arr_pixels);

    // OpenGL returns the bottom row first; PNG stores the top row first
    stbi_flip_vertically_on_write(1);
    if (stbi_write_png(a_str_path, int_w, int_h, 3, arr_pixels, int_w * 3))
        printf("Screenshot saved to %s (%dx%d)\n", a_str_path, int_w, int_h);
    else
        fprintf(stderr, "Cannot write %s\n", a_str_path);
    free(arr_pixels);
}
