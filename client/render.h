/** @file client/render.h
 * @brief Shared renderer API, including lobby portraits and session reset.
 * @details Zhikai's contributions integrate game effects, centred labels,
 * directional Ninja Adventure sprites and menu portraits with the team's renderer.
 */

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

#include <stdbool.h>
#include "net_client.h"

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
// Call after particles_init() (the vertex batch has room for every particle).
void render_init(RenderMode a_mode, int a_int_stress);

// Fit the drawing to a framebuffer of a_int_w x a_int_h pixels. With display
// scaling (e.g. 133% on Wayland) an 800 x 600 window has a larger framebuffer;
// the game is still drawn in WIDTH x HEIGHT game pixels, stretched to fill it.
void render_resize(int a_int_w, int a_int_h);

// Main is the sole network-frame consumer. Share its copy with the renderer
// and the menu; neither draws from the worker's live state.
/** @brief Share main's copied network frame with the renderer and interpolation.
 * @param a_frame Coherent frame already consumed by main; copied immediately.
 * @note Render thread only; does not read the network handoff again.
 */
void render_set_client_frame(const ClientFrame *a_frame);

/** @brief Clear received state, interpolation history, particles and event ticks.
 * @note Main/render thread only, between sessions after the worker is joined.
 */
void render_reset_session(void);

// Draw one frame: floor, stress sprites, particles, every active player's sprite and ID,
// and the banner. Does not swap buffers.
/** @brief Draw arena sprites, game effects, player labels and status text.
 * @pre A current GL context and initialised renderer/particles.
 * @note Uses the copied client frame; does not swap window buffers.
 */
void render_frame(void);

// Draw a player's atlas portrait in the lobby (player index 0..4).
// Restores GL texture state, including the short batch's texture matrix.
/** @brief Draw one character portrait for a lobby slot.
 * @param a_int_player Player index in 0..4; invalid indices draw nothing.
 * @param x Left coordinate in logical game pixels.
 * @param y Top coordinate in logical game pixels.
 * @param a_f_size Portrait side length in logical pixels.
 * @note Restores GL texture state and the short-batch texture matrix.
 */
void render_draw_portrait(int a_int_player, float x, float y, float a_f_size);

// Save the last drawn frame (the back buffer, before glfwSwapBuffers) as a PNG,
// at the framebuffer's size
void render_save_screenshot(const char *a_str_path);

#endif /* CLIENT_RENDER_H */
