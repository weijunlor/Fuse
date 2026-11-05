/** @file client/input.h
 * @brief Keyboard polling into the held-key bitmask (sent on change, #13).
 */

#ifndef CLIENT_INPUT_H
#define CLIENT_INPUT_H

#include <stdint.h>
#include <GLFW/glfw3.h>

/** @brief Read the keyboard and publish the held-key bitmask.
 * @details Arrows/WASD move, Space/Shift dash; the mask goes to
 * net_client_set_keys(), and the DDS worker sends it when it changes.
 * Opposite directions cancel out on the server. With FUSE_AUTO_MOVE=1 the
 * keyboard is ignored (see auto_move() in input.c).
 * @param a_p_window Window to poll.
 * @note Main thread (GLFW requires it).
 */
void input_poll(GLFWwindow *a_p_window);

#endif /* CLIENT_INPUT_H */
