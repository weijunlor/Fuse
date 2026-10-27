/* client/input.h
Keyboard polling to key bitmask; send on change. */

#ifndef CLIENT_INPUT_H
#define CLIENT_INPUT_H

#include <GLFW/glfw3.h>

// Direction held this frame: 'u', 'd', 'l', 'r', or 'n' for no direction.
// Written by the main thread, read by the DDS worker (under the mutex).
extern char ch_dir;

// Read the arrow keys (non-blocking) and update ch_dir. Up wins over down,
// down over left, left over right.
void input_poll(GLFWwindow *a_p_window);

#endif /* CLIENT_INPUT_H */
