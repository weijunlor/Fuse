/* client/main.c
FUSE client entry point. Based on baseline/client.c (Dr Khoo Teck Ping,
27 Aug 2026), split into modules with behaviour unchanged:

1. DDS client (net_client.c)
2. Supports max 5 clients
3. Start server, then start client one by one
4. Each client can see itself and all other clients (render.c)
5. Each client can move itself using the arrow keys (input.c)
6. Collision detection among all players and the window is supported (server)

Threads: the main thread owns the window, reads the keyboard and draws;
a DDS worker thread does all network I/O (net_client.c). */

#include <stdio.h>
#include <pthread.h>
#include <GLFW/glfw3.h>

#include "config.h"
#include "net_client.h"
#include "input.h"
#include "render.h"

int main(void)
{
	pthread_t thread;

    if (pthread_create(&thread, NULL, net_client_worker, NULL) != 0)
    {
        perror("pthread_create");
        return 1;
    }

    if (!glfwInit())
        return 1;

    GLFWwindow *window =
        glfwCreateWindow(WIDTH, HEIGHT, "DDS Client (Click within the window to control)", NULL, NULL);

    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    render_init();

    // Render loop: no frame cap; swap interval left at the driver default (as in the baseline)
    while (!glfwWindowShouldClose(window))
    {
        /* Process keyboard/window events */
        glfwPollEvents();
        input_poll(window);
        render_frame();
        glfwSwapBuffers(window);
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
