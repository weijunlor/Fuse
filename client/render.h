/* client/render.h
Batched rendering of players, bomb and HUD. */

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

// Set up a pixel-coordinate projection with (0, 0) at the top-left corner,
// load the sprite atlas and enable alpha blending. Needs a current GL context.
void render_init(void);

// Draw one frame: floor, every active player's sprite and ID, and the banner
void render_frame(void);

// Save the last drawn frame (the back buffer, before glfwSwapBuffers) as a PNG
void render_save_screenshot(const char *a_str_path);

#endif /* CLIENT_RENDER_H */
