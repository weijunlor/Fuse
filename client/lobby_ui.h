/** @file client/lobby_ui.h
 * @brief Join, cancel, leave and exit controls for the native client menu.
 * @details All functions run on the GLFW/main thread. Pairing integration is
 * supplied by the team's separate pairing UI; match admission stays on the server.
 */
#ifndef CLIENT_LOBBY_UI_H
#define CLIENT_LOBBY_UI_H

#include <GLFW/glfw3.h>
#include "net_client.h"

/** @brief Action requested by the menu; main performs the session change. */
typedef enum
{
    LOBBY_NONE,  /**< No action this frame. */
    LOBBY_JOIN,  /**< Start or retry joining a game. */
    LOBBY_LEAVE, /**< Cancel a pending join or leave the current session. */
    LOBBY_PAIR,  /**< Open the team's host-pairing flow. */
    LOBBY_EXIT   /**< Leave if needed, then close the client. */
} LobbyAction;

/** @brief Install the menu key callback and enable sticky mouse presses.
 * @param a_window GLFW window that receives menu input.
 * @note Call once before polling. The callback also forwards pairing UI keys.
 */
void lobby_ui_init(GLFWwindow *a_window);

/** @brief Convert fresh keyboard/mouse presses into one menu action.
 * @param a_window GLFW window whose events have just been polled.
 * @param a_frame Main thread's copied session/snapshot state.
 * @param a_b_running True after starting the worker until main stops and joins
 * it, including pending or rejected login states.
 * @return Requested action, or LOBBY_NONE. Does not start or stop the worker.
 */
LobbyAction lobby_ui_poll(GLFWwindow *a_window, const ClientFrame *a_frame, bool a_b_running);

/** @brief Draw the menu, waiting-room roster or in-game Leave/Exit buttons.
 * @param a_frame Main thread's copied session/snapshot state.
 * @param a_b_running True while the session worker has not been joined.
 * @param a_str_message Optional startup-error text; NULL or empty hides it.
 * @pre A current GL context with the game's pixel-coordinate projection.
 */
void lobby_ui_draw(const ClientFrame *a_frame, bool a_b_running, const char *a_str_message);

#endif /* CLIENT_LOBBY_UI_H */
