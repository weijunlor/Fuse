/* client/render.c
Batched rendering of players, bomb and HUD.

This is the first simple version: every sprite is its own glBegin/glEnd
quad (immediate mode). Optimisation #16 batches all quads into one draw
call; keep this path runnable so the two can be compared. */

#include <stdio.h>
#include <GLFW/glfw3.h>
#include <stb_image_write.h>   // from libstb-dev; implementation is in libstb.so

#include "render.h"
#include "atlas.h"
#include "net_client.h"
#include "ui.h"

// Seconds each player idle frame is shown
#define PLAYER_FRAME_S 0.4

static GLuint texture_atlas;

// Draw one sprite with its top-left corner at (x, y), at the atlas tile size
static void render_sprite(int a_int_sprite, float x, float y)
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

void render_init(void)
{
    /* Use pixel coordinates */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, WIDTH, HEIGHT, 0, -1, 1);

    texture_atlas = atlas_load();

    // Sprites have transparent pixels around them
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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

    // Players: the 64 px sprite is centred on the SQ_WIDTH hitbox
    const float f_offset = (SQ_WIDTH - ATLAS_TILE) / 2.0f;
    for(int i = 0; i < MAX_PLAYERS; i++)
    {
        pthread_mutex_lock(&mutex);
        if(arr_Players[i].b_active == true)
        {
            int int_sprite = atlas_frame(SPR_PLAYER + i, PLAYER_FRAMES, PLAYER_STEP,
                                         d_time_s, PLAYER_FRAME_S);
            render_sprite(int_sprite, arr_Players[i].x + f_offset,
                          arr_Players[i].y + f_offset);
        }
        pthread_mutex_unlock(&mutex);
    }

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
    static unsigned char arr_pixels[WIDTH * HEIGHT * 3];

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, arr_pixels);

    // OpenGL returns the bottom row first; PNG stores the top row first
    stbi_flip_vertically_on_write(1);
    if (stbi_write_png(a_str_path, WIDTH, HEIGHT, 3, arr_pixels, WIDTH * 3))
        printf("Screenshot saved to %s\n", a_str_path);
    else
        fprintf(stderr, "Cannot write %s\n", a_str_path);
}
