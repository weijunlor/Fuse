/* client/atlas.c
Texture atlas: loading and sprite lookup. */

#include <stdio.h>
#include <stdlib.h>
#include <stb_image.h>   // from libstb-dev; implementation is in libstb.so

#include "atlas.h"

GLuint atlas_load(void)
{
    int int_w, int_h, int_channels;
    // Force 4 channels (RGBA) so transparent pixels keep their alpha
    unsigned char *p_pixels = stbi_load(ATLAS_PATH, &int_w, &int_h, &int_channels, 4);
    if (p_pixels == NULL)
    {
        fprintf(stderr, "Cannot load %s: %s (start the client from the repo root)\n",
                ATLAS_PATH, stbi_failure_reason());
        exit(1);
    }
    if (int_w != ATLAS_TILE * ATLAS_COLS || int_h != ATLAS_TILE * ATLAS_ROWS)
    {
        fprintf(stderr, "%s is %dx%d, expected %dx%d\n", ATLAS_PATH, int_w, int_h,
                ATLAS_TILE * ATLAS_COLS, ATLAS_TILE * ATLAS_ROWS);
        exit(1);
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    // Nearest-neighbour: pixel art stays sharp instead of being blurred
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    // Image row 0 is the top of the atlas and becomes v = 0, which matches
    // the client's top-left origin, so no vertical flip is needed
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, int_w, int_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, p_pixels);

    // OpenGL has its own copy now
    stbi_image_free(p_pixels);
    return texture;
}

void atlas_uv(int a_int_sprite, float *u0, float *v0, float *u1, float *v1)
{
    // The atlas is not square, so a tile's width and height in texture
    // coordinates differ
    const float f_tile_u = 1.0f / ATLAS_COLS;
    const float f_tile_v = 1.0f / ATLAS_ROWS;
    int int_col = a_int_sprite % ATLAS_COLS;
    int int_row = a_int_sprite / ATLAS_COLS;

    *u0 = int_col * f_tile_u;
    *v0 = int_row * f_tile_v;
    *u1 = *u0 + f_tile_u;
    *v1 = *v0 + f_tile_v;
}

void atlas_texel(int a_int_sprite, int *x, int *y)
{
    *x = (a_int_sprite % ATLAS_COLS) * ATLAS_TILE;
    *y = (a_int_sprite / ATLAS_COLS) * ATLAS_TILE;
}

int atlas_frame(int a_int_first, int a_int_frames, int a_int_step,
                double a_d_time_s, double a_d_frame_s)
{
    int int_frame = (int)(a_d_time_s / a_d_frame_s) % a_int_frames;
    return a_int_first + int_frame * a_int_step;
}
