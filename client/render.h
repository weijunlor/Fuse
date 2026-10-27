/* client/render.h
Batched rendering of players, bomb and HUD. */

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

#include <stdbool.h>

// How sprites are sent to OpenGL (optimisation #16). Every step stays
// selectable (--render <name>) so each can be measured against the one before.
typedef enum
{
    RENDER_IMMEDIATE,       // "immediate": one glBegin/glEnd per sprite (#16a before)
    RENDER_BATCHED_FLOAT,   // "float": one glDrawArrays per frame, float vertices, 16 B (#16a after, #16b before)
    RENDER_BATCHED_SHORT    // "short": as "float" with short vertices, 8 B (#16b after; default)
} RenderMode;

// Mode from its name ("immediate", "float", "short"); -1 if unknown
int render_mode_from_name(const char *a_str_name);
const char *render_mode_name(RenderMode a_mode);

// Set up a pixel-coordinate projection with (0, 0) at the top-left corner,
// load the sprite atlas and enable alpha blending. Needs a current GL context.
// a_int_stress adds that many animated test sprites per frame.
void render_init(RenderMode a_mode, int a_int_stress);

// Fit the drawing to a framebuffer of a_int_w x a_int_h pixels. With display
// scaling (e.g. 133% on Wayland) an 800 x 600 window has a larger framebuffer;
// the game is still drawn in WIDTH x HEIGHT game pixels, stretched to fill it.
void render_resize(int a_int_w, int a_int_h);

// Draw one frame: floor, stress sprites, every active player's sprite and ID,
// and the banner. Does not swap buffers.
void render_frame(void);

// Save the last drawn frame (the back buffer, before glfwSwapBuffers) as a PNG,
// at the framebuffer's size
void render_save_screenshot(const char *a_str_path);

#endif /* CLIENT_RENDER_H */
