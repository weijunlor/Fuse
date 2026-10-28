/* client/input.h
Keyboard polling to key bitmask; send on change. */

#ifndef CLIENT_INPUT_H
#define CLIENT_INPUT_H

#include <stdint.h>
#include <GLFW/glfw3.h>

// Keys held this frame: KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT (types.h),
// 0 for none. Written by the main thread, read by the DDS worker (under the mutex).
extern uint8_t u8_keys;

// Read the arrow keys (non-blocking) and update u8_keys. Several keys can be
// held at once (diagonal movement); opposite keys cancel out on the server.
void input_poll(GLFWwindow *a_p_window);

#endif /* CLIENT_INPUT_H */
