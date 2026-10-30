/* client/input.h
Keyboard polling to key bitmask; send on change. */

#ifndef CLIENT_INPUT_H
#define CLIENT_INPUT_H

#include <stdint.h>
#include <GLFW/glfw3.h>

// Read arrows/WASD and Space/Shift, then publish the held-key bitmask with
// net_client_set_keys(). Opposite directions cancel out on the server.
void input_poll(GLFWwindow *a_p_window);

#endif /* CLIENT_INPUT_H */
