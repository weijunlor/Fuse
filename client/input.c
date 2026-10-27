/* client/input.c
Keyboard polling to key bitmask; send on change. */

#include "input.h"
#include "net_client.h"

// Direction - n means no direction
char ch_dir = 'n';

void input_poll(GLFWwindow *a_p_window)
{
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
