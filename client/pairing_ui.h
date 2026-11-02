/* Main-thread UI and a supervised background enrollment worker. */
#ifndef CLIENT_PAIRING_UI_H
#define CLIENT_PAIRING_UI_H
#include <stdbool.h>
#include <GLFW/glfw3.h>
bool pairing_profile_load(void);
void pairing_ui_init(GLFWwindow *a_window, bool a_legacy_ready);
bool pairing_ui_ready(void);
bool pairing_ui_visible(void);
const char *pairing_ui_host(void);
void pairing_ui_open(void);
void pairing_ui_key(GLFWwindow *a_window, int a_key, int a_action, int a_mods);
void pairing_ui_poll(GLFWwindow *a_window);
void pairing_ui_draw(void);
void pairing_ui_shutdown(void);
#endif
