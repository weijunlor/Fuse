/* client/input.c
Keyboard polling to key bitmask; send on change. */

#include <stdlib.h>

#include "input.h"
#include "net_client.h"
#include "types.h"

// Keys held (KEY_* bitmask); 0 means none
uint8_t u8_keys = 0;

// Seconds each direction is held in auto-move mode
#define AUTO_MOVE_S 1.0

/* Store the newly read keys. If they changed, wake the DDS worker (#2): it
sleeps in a waitset and sends the new keys at once instead of finding them
on its next pass. */
static void set_keys(uint8_t a_u8_new)
{
	pthread_mutex_lock(&mutex);
	bool b_changed = u8_keys != a_u8_new;
	u8_keys = a_u8_new;
	pthread_mutex_unlock(&mutex);
	if (b_changed)
		net_client_wake();
}

/* Auto-move (for measurements): with FUSE_AUTO_MOVE=1 in the environment the
keyboard is ignored and the player holds right, down, left, up for
AUTO_MOVE_S seconds each, round and round, so profiling runs include the input
messages a held key sends without anyone at the keyboard.
Returns true if auto-move is on. */
static bool auto_move(void)
{
	static int int_auto = -1;   // -1: environment not read yet
	if (int_auto < 0)
	{
		const char *str_auto = getenv("FUSE_AUTO_MOVE");
		int_auto = (str_auto != NULL && str_auto[0] == '1');
	}
	if (!int_auto)
		return false;

	static const uint8_t arr_u8_dirs[4] = { KEY_RIGHT, KEY_DOWN, KEY_LEFT, KEY_UP };
	set_keys(arr_u8_dirs[(int)(glfwGetTime() / AUTO_MOVE_S) % 4]);
	return true;
}

void input_poll(GLFWwindow *a_p_window)
{
	if (auto_move())
		return;

	/* Non-blocking keyboard input: one bit per held key. Arrows or WASD move;
	Space or either Shift dashes (the server starts a dash when KEY_DASH goes
	from up to down, so holding it does not repeat the dash). */
	uint8_t u8_new = 0;
	if (glfwGetKey(a_p_window, GLFW_KEY_UP) == GLFW_PRESS ||
	    glfwGetKey(a_p_window, GLFW_KEY_W) == GLFW_PRESS)
		u8_new |= KEY_UP;
	if (glfwGetKey(a_p_window, GLFW_KEY_DOWN) == GLFW_PRESS ||
	    glfwGetKey(a_p_window, GLFW_KEY_S) == GLFW_PRESS)
		u8_new |= KEY_DOWN;
	if (glfwGetKey(a_p_window, GLFW_KEY_LEFT) == GLFW_PRESS ||
	    glfwGetKey(a_p_window, GLFW_KEY_A) == GLFW_PRESS)
		u8_new |= KEY_LEFT;
	if (glfwGetKey(a_p_window, GLFW_KEY_RIGHT) == GLFW_PRESS ||
	    glfwGetKey(a_p_window, GLFW_KEY_D) == GLFW_PRESS)
		u8_new |= KEY_RIGHT;
	if (glfwGetKey(a_p_window, GLFW_KEY_SPACE) == GLFW_PRESS ||
	    glfwGetKey(a_p_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
	    glfwGetKey(a_p_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
		u8_new |= KEY_DASH;

	set_keys(u8_new);
}
