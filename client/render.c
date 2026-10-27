/* client/render.c
Batched rendering of players, bomb and HUD.

Optimisation #16 (batched drawing). Two ways to draw sprites, chosen at start-up:
- Immediate (the first simple version, `--render-immediate`): every sprite is
  its own glBegin/glEnd, i.e. 4 glTexCoord + 4 glVertex driver calls per
  sprite.
- Batched (default): sprites are appended to one vertex array in memory and
  drawn with a single glDrawArrays call per frame, so the driver is called a
  few times per frame instead of ~9 times per sprite.
Text is still drawn per string (ui.c) in both modes. */

#include <stdio.h>
#include <stdlib.h>
#include <GLFW/glfw3.h>
#include <stb_image_write.h>   // from libstb-dev; implementation is in libstb.so

#include "render.h"
#include "atlas.h"
#include "net_client.h"
#include "ui.h"

// Seconds each animation frame is shown
#define PLAYER_FRAME_S 0.4
#define STRESS_FRAME_S 0.1

// Floor tiles needed to cover the window (the tile size divides the window
// size exactly, so this is 16 x 12)
#define FLOOR_TILES ((WIDTH / ATLAS_TILE) * (HEIGHT / ATLAS_TILE))

// One corner of a sprite quad. Position and texture coordinates are stored
// together (interleaved), so each corner is one 16-byte read for the driver.
typedef struct
{
    float x, y;
    float u, v;
} QuadVertex;

// Animated test sprite for --stress (stands in for particles and embers)
typedef struct
{
    float x, y;
    int int_first;      // first frame's sprite ID
    int int_frames;     // number of frames
    double d_phase_s;   // animation offset, so the sprites do not blink in sync
} StressSprite;

static GLuint texture_atlas;
static bool b_immediate;

// Batch: vertices of every sprite this frame, drawn by batch_flush()
static QuadVertex *arr_batch;
static int int_batch_len;   // vertices in use
static int int_batch_cap;   // vertices allocated

static StressSprite *arr_stress;
static int int_num_stress;

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

// Draw one sprite with its top-left corner at (x, y), at the atlas tile size
static void render_sprite(int a_int_sprite, float x, float y)
{
    if (b_immediate)
        sprite_immediate(a_int_sprite, x, y);
    else
        sprite_batched(a_int_sprite, x, y);
}

// Batched path: draw every queued sprite with one call, then empty the batch
static void batch_flush(void)
{
    if (b_immediate || int_batch_len == 0)
        return;

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, sizeof(QuadVertex), &arr_batch[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(QuadVertex), &arr_batch[0].u);
    glDrawArrays(GL_QUADS, 0, int_batch_len);
    // Text (ui.c) draws with its own vertex array; a texture-coordinate array
    // left enabled would make the driver read past the end of ours
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    int_batch_len = 0;
}

void render_init(bool a_b_immediate, int a_int_stress)
{
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

    b_immediate = a_b_immediate;

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

    // Batch capacity: floor + players + stress sprites, 4 vertices each
    int_batch_cap = (FLOOR_TILES + MAX_PLAYERS + int_num_stress) * 4;
    arr_batch = malloc((size_t)int_batch_cap * sizeof(QuadVertex));
    if (arr_batch == NULL || arr_stress == NULL)
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

    // Players: the sprite is the same size as the SQ_WIDTH hitbox
    for(int i = 0; i < MAX_PLAYERS; i++)
    {
        pthread_mutex_lock(&mutex);
        if(arr_Players[i].b_active == true)
        {
            int int_sprite = atlas_frame(SPR_PLAYER + i, PLAYER_FRAMES, PLAYER_STEP,
                                         d_time_s, PLAYER_FRAME_S);
            render_sprite(int_sprite, arr_Players[i].x, arr_Players[i].y);
        }
        pthread_mutex_unlock(&mutex);
    }

    // Batched mode: everything above is drawn here, in one call
    batch_flush();

    /* ----------------------------------------------------- */
    /* Text (untextured quads)                               */
    /* ----------------------------------------------------- */
    glDisable(GL_TEXTURE_2D);
    glColor3f(0.15f, 0.15f, 0.2f);

    char str_banner[100] = "Waiting for server ...";
    if (int_player_id != -1)
    {
        snprintf(str_banner, sizeof(str_banner), "Player %d", int_player_id);
    }
    ui_draw_text(320, 20, 3.0f, str_banner);

    for(int i = 0; i < MAX_PLAYERS; i++)
    {
        pthread_mutex_lock(&mutex);
        if(arr_Players[i].b_active == true)
        {
            // Label the player ID above the sprite
            char str_player[2] = "";
            snprintf(str_player, sizeof(str_player), "%d", i+1);
            ui_draw_text(arr_Players[i].x + SQ_WIDTH / 2 - 7, arr_Players[i].y - 30,
                         3.0f, str_player);
        }
        pthread_mutex_unlock(&mutex);
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
