/* client/render.h
Batched rendering of players, bomb and HUD. */

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

#include <stdbool.h>

// Set up a pixel-coordinate projection with (0, 0) at the top-left corner,
// load the sprite atlas and enable alpha blending. Needs a current GL context.
// a_b_immediate selects the slow one-call-per-sprite path (optimisation #16
// "before"); a_int_stress adds that many animated test sprites per frame.
void render_init(bool a_b_immediate, int a_int_stress);

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
