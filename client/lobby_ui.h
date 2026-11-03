/* Native client menu. All functions run on the GLFW/main thread. */
#ifndef CLIENT_LOBBY_UI_H
#define CLIENT_LOBBY_UI_H

#include <GLFW/glfw3.h>
#include "net_client.h"

typedef enum
{
    LOBBY_NONE,
    LOBBY_JOIN,
    LOBBY_LEAVE,
    LOBBY_PAIR,
    LOBBY_EXIT
} LobbyAction;

void lobby_ui_init(GLFWwindow *a_window);
LobbyAction lobby_ui_poll(GLFWwindow *a_window, const ClientFrame *a_frame, bool a_b_running);
void lobby_ui_draw(const ClientFrame *a_frame, bool a_b_running, const char *a_str_message);

#endif /* CLIENT_LOBBY_UI_H */
