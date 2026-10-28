/* client/input.c
Keyboard polling to key bitmask; send on change. */

#include <stdlib.h>

#include "input.h"
#include "net_client.h"

// Direction - n means no direction
char ch_dir = 'n';

// Seconds each direction is held in auto-move mode
#define AUTO_MOVE_S 1.0

/* Auto-move (for measurements): with FUSE_AUTO_MOVE=1 in the environment the
keyboard is ignored and the player holds right, down, left, up for
AUTO_MOVE_S seconds each, round and round, so profiling runs include the input
messages a held key sends (~100 per second) without anyone at the keyboard.
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

	static const char arr_dirs[4] = { 'r', 'd', 'l', 'u' };
	char ch_new = arr_dirs[(int)(glfwGetTime() / AUTO_MOVE_S) % 4];
	pthread_mutex_lock(&mutex);
	ch_dir = ch_new;
	pthread_mutex_unlock(&mutex);
	return true;
}

void input_poll(GLFWwindow *a_p_window)
{
	if (auto_move())
		return;

	/* Non-blocking keyboard input */
	if (glfwGetKey(a_p_window, GLFW_KEY_UP) == GLFW_PRESS)
	{
		pthread_mutex_lock(&mutex);
		ch_dir = 'u';
		pthread_mutex_unlock(&mutex);
	}

	else if (glfwGetKey(a_p_window, GLFW_KEY_DOWN) == GLFW_PRESS)
	{
		pthread_mutex_lock(&mutex);
		ch_dir = 'd';
		pthread_mutex_unlock(&mutex);
	}

	else if (glfwGetKey(a_p_window, GLFW_KEY_LEFT) == GLFW_PRESS)
	{
		pthread_mutex_lock(&mutex);
		ch_dir = 'l';
		pthread_mutex_unlock(&mutex);
	}

	else if (glfwGetKey(a_p_window, GLFW_KEY_RIGHT) == GLFW_PRESS)
	{
		pthread_mutex_lock(&mutex);
		ch_dir = 'r';
		pthread_mutex_unlock(&mutex);
	}

	else
	{
		pthread_mutex_lock(&mutex);
		ch_dir = 'n';
		pthread_mutex_unlock(&mutex);
	}
}
