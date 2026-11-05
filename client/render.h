/** @file client/render.h
 * @brief Shared renderer API, including lobby portraits and session reset.
 * @details Zhikai's contributions integrate game effects, centred labels,
 * directional Ninja Adventure sprites and menu portraits with the team's renderer.
 */

#ifndef CLIENT_RENDER_H
#define CLIENT_RENDER_H

#include <stdbool.h>
#include "net_client.h"

/** @brief How sprites are sent to OpenGL (optimisation #16).
 * @details Every step stays selectable (--render <name>) so each can be
 * measured against the one before.
 */
typedef enum
{
    RENDER_IMMEDIATE,       ///< "immediate": one glBegin/glEnd per sprite (#16a before)
    RENDER_BATCHED_FLOAT,   ///< "float": one glDrawArrays per frame, float vertices, 16 B (#16a after, #16b before)
    RENDER_BATCHED_SHORT    ///< "short": as "float" with short vertices, 8 B (#16b after; default)
} RenderMode;

/** @brief Look up a render mode by its command-line name.
 * @param a_str_name "immediate", "float" or "short".
 * @return The matching RenderMode, or -1 if the name is unknown.
 */
int render_mode_from_name(const char *a_str_name);

/** @brief Command-line name of a render mode.
 * @param a_mode Mode to name.
 * @return Static string, e.g. "short".
 */
const char *render_mode_name(RenderMode a_mode);

/** @brief Set up the renderer.
 * @details Sets a pixel-coordinate projection with (0, 0) at the top-left
 * corner, loads the sprite atlas and enables alpha blending.
 * @param a_mode Sprite submission path (#16).
 * @param a_int_stress Number of extra animated test sprites drawn per frame
 *        (0 in normal play; used by bench/render.sh).
 * @pre A current GL context. Call after particles_init(): the vertex batch is
 *      sized to hold every particle.
 */
void render_init(RenderMode a_mode, int a_int_stress);

/** @brief Fit the drawing to the window's framebuffer.
 * @details With display scaling (e.g. 133% on Wayland) an 800 x 600 window has
 * a larger framebuffer; the game is still drawn in WIDTH x HEIGHT game pixels,
 * stretched to fill it.
 * @param a_int_w Framebuffer width in device pixels.
 * @param a_int_h Framebuffer height in device pixels.
 */
void render_resize(int a_int_w, int a_int_h);

/** @brief Share main's copied network frame with the renderer and interpolation.
 * @details Main is the sole network-frame consumer; the renderer and the menu
 * share its copy and neither draws from the worker's live state.
 * @param a_frame Coherent frame already consumed by main; copied immediately.
 * @note Render thread only; does not read the network handoff again.
 */
void render_set_client_frame(const ClientFrame *a_frame);

/** @brief Clear received state, interpolation history, particles and event ticks.
 * @note Main/render thread only, between sessions after the worker is joined.
 */
void render_reset_session(void);

/** @brief Draw arena sprites, game effects, player labels and status text.
 * @details Draws the floor, stress sprites, particles, every active player's
 * sprite and label, and the banner.
 * @pre A current GL context and initialised renderer/particles.
 * @note Uses the copied client frame; does not swap window buffers.
 */
void render_frame(void);

/** @brief Draw one character portrait for a lobby slot.
 * @param a_int_player Player index in 0..4; invalid indices draw nothing.
 * @param x Left coordinate in logical game pixels.
 * @param y Top coordinate in logical game pixels.
 * @param a_f_size Portrait side length in logical pixels.
 * @note Restores GL texture state and the short-batch texture matrix.
 */
void render_draw_portrait(int a_int_player, float x, float y, float a_f_size);

/** @brief Save the last drawn frame as a PNG (F12 or FUSE_SCREENSHOT).
 * @details Reads the back buffer (call before glfwSwapBuffers()) at the
 * framebuffer's size.
 * @param a_str_path Output file path.
 */
void render_save_screenshot(const char *a_str_path);

#endif /* CLIENT_RENDER_H */
