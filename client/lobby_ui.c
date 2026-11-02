/* Simple lobby drawn with the existing OpenGL text and rectangle primitives.
   A joined lobby shows the server's five slots; the server owns admission,
   countdown and match start. The menu never changes authoritative game state. */
#include <stdio.h>
#include "lobby_ui.h"
#include "ui.h"
#include "render.h"
#include "pairing_ui.h"

typedef struct
{
    float x, y, w, h;
} Button;

static const Button button_primary = {180, 465, 210, 48};
static const Button button_secondary = {410, 465, 210, 48};
static const Button button_leave = {560, 535, 140, 36};
static const Button button_exit = {710, 535, 70, 36};
static double d_mouse_x, d_mouse_y;
static bool b_mouse_down, b_enter_pressed, b_escape_pressed;

static bool in_button(Button a_button)
{
    return d_mouse_x >= a_button.x && d_mouse_x < a_button.x + a_button.w &&
           d_mouse_y >= a_button.y && d_mouse_y < a_button.y + a_button.h;
}

// Latch presses: a quick press/release between two rendered frames still
// counts once. GLFW_REPEAT is ignored, so holding Enter cannot rejoin.
static void on_key(GLFWwindow *a_window, int a_int_key, int a_int_scancode,
                   int a_int_action, int a_int_mods)
{
    (void)a_window;
    (void)a_int_scancode;
    if (pairing_ui_visible()) { pairing_ui_key(a_window, a_int_key, a_int_action, a_int_mods); return; }
    (void)a_int_mods;
    if (a_int_action != GLFW_PRESS)
        return;
    if (a_int_key == GLFW_KEY_ENTER || a_int_key == GLFW_KEY_KP_ENTER)
        b_enter_pressed = true;
    if (a_int_key == GLFW_KEY_ESCAPE)
        b_escape_pressed = true;
}

void lobby_ui_init(GLFWwindow *a_window)
{
    b_mouse_down = b_enter_pressed = b_escape_pressed = false;
    glfwSetKeyCallback(a_window, on_key);
    glfwSetInputMode(a_window, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);
}

static bool in_arena(const ClientFrame *a_frame, bool a_b_running)
{
    return a_b_running && a_frame->int_player_id > 0 &&
           a_frame->snapshot.phase != PHASE_LOBBY;
}

LobbyAction lobby_ui_poll(GLFWwindow *a_window, const ClientFrame *a_frame, bool a_b_running)
{
    // Cursor positions are in window coordinates, not framebuffer pixels.
    // Scale them to the same logical 800x600 projection used for drawing.
    int int_w, int_h;
    glfwGetWindowSize(a_window, &int_w, &int_h);
    glfwGetCursorPos(a_window, &d_mouse_x, &d_mouse_y);
    if (int_w > 0 && int_h > 0)
    {
        d_mouse_x *= (double)WIDTH / int_w;
        d_mouse_y *= (double)HEIGHT / int_h;
    }
    bool b_down = glfwGetMouseButton(a_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool b_click = b_down && !b_mouse_down;
    b_mouse_down = b_down;
    bool b_enter = b_enter_pressed, b_escape = b_escape_pressed;
    b_enter_pressed = b_escape_pressed = false;

    if (b_escape)
        return a_b_running ? LOBBY_LEAVE : LOBBY_EXIT;
    bool b_retry = a_b_running && a_frame->connection == CLIENT_REJECTED;
    if (b_enter && (!a_b_running || b_retry))
        return LOBBY_JOIN;
    if (!b_click)
        return LOBBY_NONE;
    if (!a_b_running && pairing_ui_ready() && d_mouse_x >= 180 && d_mouse_x <= 620 &&
        d_mouse_y >= 535 && d_mouse_y <= 570) return LOBBY_PAIR;
    bool b_arena = in_arena(a_frame, a_b_running);
    if (in_button(b_arena ? button_exit : button_secondary))
        return LOBBY_EXIT;
    if (in_button(b_arena ? button_leave : button_primary))
        return !a_b_running || b_retry ? LOBBY_JOIN : LOBBY_LEAVE;
    return LOBBY_NONE;
}

static void rectangle(float x, float y, float w, float h, float r, float g, float b)
{
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

static void text(float x, float y, float a_f_scale, const char *a_str_text)
{
    glColor3f(0.91f, 0.94f, 0.98f);
    ui_draw_text(x, y, a_f_scale, a_str_text);
}

static void draw_button(Button a_button, const char *a_str_label, bool a_b_primary)
{
    float f_hover = in_button(a_button) ? 0.08f : 0.0f;
    rectangle(a_button.x, a_button.y, a_button.w, a_button.h,
              (a_b_primary ? 0.10f : 0.20f) + f_hover,
              (a_b_primary ? 0.43f : 0.25f) + f_hover,
              (a_b_primary ? 0.32f : 0.34f) + f_hover);
    text(a_button.x + 14, a_button.y + (a_button.h - 12) / 2, 1.5f, a_str_label);
}

void lobby_ui_draw(const ClientFrame *a_frame, bool a_b_running, const char *a_str_message)
{
    glDisable(GL_TEXTURE_2D);
    if (in_arena(a_frame, a_b_running))
    {
        draw_button(button_leave, "Leave [Esc]", false);
        draw_button(button_exit, "Exit", false);
        return;
    }

    // A full menu hides the arena while disconnected or waiting in the lobby.
    rectangle(0, 0, WIDTH, HEIGHT, 0.035f, 0.055f, 0.09f);
    rectangle(140, 55, 520, 490, 0.075f, 0.105f, 0.16f);
    rectangle(140, 55, 520, 5, 0.25f, 0.85f, 0.60f);
    text(180, 95, 4.0f, "FUSE");
    text(180, 146, 1.5f, "Pass the bomb. Survive the fuse.");

    const char *str_status = "Join a game to enter the waiting room.";
    char str_host_status[96];
    if (!a_b_running && pairing_ui_ready() && *pairing_ui_host()) {
        snprintf(str_host_status, sizeof str_host_status, "Ready to join %s", pairing_ui_host());
        str_status = str_host_status;
    }
    if (!a_b_running && !pairing_ui_ready()) str_status = "Pair this PC with your host to get started.";
    if (a_b_running)
    {
        switch (a_frame->connection)
        {
        case CLIENT_JOINED: str_status = "Waiting room"; break;
        case CLIENT_WAITING_MATCH: str_status = "Match in progress. Waiting for next lobby..."; break;
        case CLIENT_FULL: str_status = "Lobby full. Retrying automatically..."; break;
        case CLIENT_REJECTED: str_status = "Login rejected. Check credentials, then retry."; break;
        default: str_status = "Connecting... Waiting for server."; break;
        }
    }
    text(180, 190, 1.5f, str_status);

    if (a_b_running && a_frame->connection == CLIENT_JOINED)
    {
        int int_count = 0;
        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            const PlayerState *p_player = &a_frame->snapshot.players[i];
            bool b_active = (p_player->flags & PF_ACTIVE) != 0;
            char str_slot[48];
            if (b_active)
            {
                int_count++;
                snprintf(str_slot, sizeof str_slot, "%s %d%s",
                         (p_player->flags & PF_BOT) ? "Bot" : "Player", i + 1,
                         a_frame->int_player_id == i + 1 ? "  (you)" : "");
            }
            else
                snprintf(str_slot, sizeof str_slot, "Slot %d - waiting", i + 1);
            rectangle(180, 232 + i * 35, 440, 29, 0.11f, 0.15f, 0.22f);
            render_draw_portrait(i, 185, 232 + i * 35, 29);
            text(225, 240 + i * 35, 1.5f, str_slot);
        }
        char str_count[96];
        if (a_frame->snapshot.timer_ms > 0)
            snprintf(str_count, sizeof str_count, "%d/%d players - starting in %.1fs",
                     int_count, MAX_PLAYERS, a_frame->snapshot.timer_ms / 1000.0);
        else
            snprintf(str_count, sizeof str_count, "%d/%d players - need at least 2 to start",
                     int_count, MAX_PLAYERS);
        text(180, 423, 1.5f, str_count);
    }
    else
    {
        text(180, 260, 1.7f, "2-5 players. One bomb.");
        text(180, 303, 1.5f, "Move: arrows / WASD");
        text(180, 334, 1.5f, "Dash: Space / Shift");
        text(180, 365, 1.5f, "Leave a game: Esc");
        for (int i = 0; i < MAX_PLAYERS; i++)
            render_draw_portrait(i, 180 + i * 80, 385, 40);
        if (a_str_message != NULL && *a_str_message != '\0')
            text(180, 435, 1.3f, a_str_message);
    }

    bool b_retry = a_b_running && a_frame->connection == CLIENT_REJECTED;
    const char *str_primary = !a_b_running ? (pairing_ui_ready() ? "Join Game [Enter]" : "Pair PC [Enter]") :
                              b_retry ? "Retry [Enter]" :
                              a_frame->int_player_id > 0 ? "Leave Game [Esc]" : "Cancel [Esc]";
    draw_button(button_primary, str_primary, !a_b_running || b_retry);
    draw_button(button_secondary, "Exit", false);
    if (!a_b_running && pairing_ui_ready()) {
        const Button pair_button = {180, 535, 440, 34};
        draw_button(pair_button, "Pair with another host", false);
    }
}
