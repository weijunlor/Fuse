/* client/input.h
Keyboard polling to key bitmask; send on change. */

#ifndef CLIENT_INPUT_H
#define CLIENT_INPUT_H

#include <stdint.h>
#include <GLFW/glfw3.h>

// Read the arrow keys and publish their bitmask with net_client_set_keys(). Several keys can be
// held at once (diagonal movement); opposite keys cancel out on the server.
void input_poll(GLFWwindow *a_p_window);

#endif /* CLIENT_INPUT_H */
