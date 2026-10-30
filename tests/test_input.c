/* Exercise the real input mapping and wake-on-change path without a window. */
#include <assert.h>
#include <stdio.h>
#include "../client/input.c"

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int arr_pressed[GLFW_KEY_LAST + 1];
static int int_wakes;

void net_client_wake(void)
{
	int_wakes++;
}

int glfwGetKey(GLFWwindow *a_p_window, int a_int_key)
{
	(void)a_p_window;
	return arr_pressed[a_int_key];
}

double glfwGetTime(void)
{
	return 0;
}

int main(void)
{
	unsetenv("FUSE_AUTO_MOVE");
	const int arr_dash[] = {GLFW_KEY_SPACE, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT};
	for (unsigned int i = 0; i < sizeof arr_dash / sizeof arr_dash[0]; i++)
	{
		arr_pressed[GLFW_KEY_D] = arr_pressed[arr_dash[i]] = GLFW_PRESS;
		int int_before = int_wakes;
		input_poll(NULL);
		assert(u8_keys == (KEY_RIGHT | KEY_DASH) && int_wakes == int_before + 1);
		input_poll(NULL);
		assert(int_wakes == int_before + 1); // unchanged keys must not keep waking
		arr_pressed[arr_dash[i]] = GLFW_RELEASE;
		input_poll(NULL);
		assert(u8_keys == KEY_RIGHT && int_wakes == int_before + 2);
	}
	arr_pressed[GLFW_KEY_D] = GLFW_RELEASE;
	input_poll(NULL);
	assert(u8_keys == 0);
	puts("PASS dash/movement/release mapping and wake only on key changes");
	return 0;
}
