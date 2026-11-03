/* Enrollment uses a separate process group. Closing/cancelling the window stops
   only that worker and its storage subprocesses; DDS starts after activation. */
#define _POSIX_C_SOURCE 200809L
#include "pairing_ui.h"
#include "ui.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/crypto.h>

extern char **environ;
static bool b_ready, b_visible, b_busy, b_thread, b_enter, b_escape, b_mouse;
static bool b_selected;
static int int_focus;
static char arr_host[16], arr_code[49], arr_saved_host[16], arr_message[256];
static pthread_t worker_thread;
static pthread_mutex_t worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pid_t worker_pid;
static bool b_cancel, b_done, b_success;
static char arr_profile[16384], arr_worker_message[256];

static int run_child(char *const args[], const char *input, char *output, size_t capacity, bool cancellable)
{
    int in[2], out[2];
    if (pipe(in)) return -1;
    if (pipe(out)) { close(in[0]); close(in[1]); return -1; }
    for (int i = 0; i < 2; i++) { fcntl(in[i], F_SETFD, FD_CLOEXEC); fcntl(out[i], F_SETFD, FD_CLOEXEC); }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, out[1], 2);
    for (int i = 0; i < 2; i++) { posix_spawn_file_actions_addclose(&actions, in[i]); posix_spawn_file_actions_addclose(&actions, out[i]); }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, args[0], &actions, &attributes, args, environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(in[0]); close(out[1]);
    if (rc) { close(in[1]); close(out[0]); return -1; }
    if (cancellable) {
        pthread_mutex_lock(&worker_mutex);
        worker_pid = pid;
        if (b_cancel) kill(-pid, SIGTERM);
        pthread_mutex_unlock(&worker_mutex);
    }
    if (input) {
        size_t length = strlen(input);
        // Codes are shorter than PIPE_BUF; a worker blocks SIGPIPE before this.
        if (write(in[1], input, length) != (ssize_t)length) rc = -1;
    }
    close(in[1]);
    size_t used = 0;
    for (;;) {
        char buffer[1024];
        ssize_t count = read(out[0], buffer, sizeof buffer);
        if (count == 0) break;
        if (count < 0) { if (errno == EINTR) continue; break; }
        size_t copy = (size_t)count;
        if (copy > capacity - used - 1) copy = capacity - used - 1;
        memcpy(output + used, buffer, copy); used += copy;
    }
    output[used] = 0; close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (cancellable) { pthread_mutex_lock(&worker_mutex); worker_pid = 0; pthread_mutex_unlock(&worker_mutex); }
    return rc ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool apply_profile(char *profile)
{
    // The helper supplies only these non-secret environment settings. Reject a
    // truncated/unknown record rather than partially activating a connection.
    const char *keys[] = {"FUSE_SERVER_IP", "FUSE_DDS_IDENTITY", "FUSE_USERNAME", "FUSE_SECURITY_DIR",
                          "FUSE_AUTH_DIR", "CYCLONEDDS_URI", "FUSE_PASSWORD_FILE"};
    char *values[7] = {0}, *save = NULL;
    for (char *line = strtok_r(profile, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *equals = strchr(line, '=');
        if (!equals) return false;
        *equals++ = 0;
        bool known = false;
        for (int i = 0; i < 7; i++) if (!strcmp(line, keys[i])) { if (values[i]) return false; values[i] = equals; known = true; }
        if (!known) return false;
    }
    for (int i = 0; i < 7; i++) if (!values[i] || !*values[i]) return false;
    struct in_addr address;
    if (strlen(values[0]) >= sizeof arr_saved_host || inet_pton(AF_INET, values[0], &address) != 1) return false;
    for (int i = 0; i < 7; i++) if (setenv(keys[i], values[i], 1)) return false;
    setenv("FUSE_DDS_SECURITY", "on", 1); setenv("FUSE_AUTH", "on", 1);
    snprintf(arr_saved_host, sizeof arr_saved_host, "%s", values[0]);
    b_ready = true;
    return true;
}

bool pairing_profile_load(void)
{
    char profile[16384];
    char *args[] = {"python3", "security/scripts/pairing_store.py", "activate", NULL};
    return run_child(args, NULL, profile, sizeof profile, false) == 0 && apply_profile(profile);
}

typedef struct { char host[16], code[50]; } PairRequest;
static void *pair_worker(void *arg)
{
    PairRequest *request = arg;
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    char message[2048] = {0}, profile[16384] = {0};
    char *args[] = {"./build/pairing", "client", request->host, NULL};
    int rc = run_child(args, request->code, message, sizeof message, true);
    OPENSSL_cleanse(request->code, sizeof request->code); free(request);
    bool committed = rc == 0 && strstr(message, "Paired. Your connection is saved.") != NULL;
    if (committed) {
        char *activate[] = {"python3", "security/scripts/pairing_store.py", "activate-saved", NULL};
        rc = run_child(activate, NULL, profile, sizeof profile, false);
        if (rc != 0) snprintf(message, sizeof message, "Saved connection could not be activated. Check your local setup.");
    }
    pthread_mutex_lock(&worker_mutex);
    if (committed) b_cancel = false;
    b_success = committed && rc == 0;
    snprintf(arr_worker_message, sizeof arr_worker_message, "%s", b_cancel ? "Pairing cancelled." :
             b_success ? "Paired. Your connection is saved." : *message ? message : "Pairing could not start. Check the host and try again.");
    if (b_success) memcpy(arr_profile, profile, sizeof profile);
    b_done = true;
    pthread_mutex_unlock(&worker_mutex);
    return NULL;
}

static void start_pair(void)
{
    struct in_addr address;
    if (inet_pton(AF_INET, arr_host, &address) != 1) { snprintf(arr_message, sizeof arr_message, "Enter the host's IPv4 address."); return; }
    if (strlen(arr_code) != 48 || strncmp(arr_code, "FUSE-", 5)) { snprintf(arr_message, sizeof arr_message, "Paste the complete code displayed by the host."); return; }
    PairRequest *request = calloc(1, sizeof *request);
    if (!request) return;
    snprintf(request->host, sizeof request->host, "%s", arr_host);
    snprintf(request->code, sizeof request->code, "%s\n", arr_code);
    memset(arr_code, 0, sizeof arr_code);
    pthread_mutex_lock(&worker_mutex); b_cancel = b_done = b_success = false; worker_pid = 0; pthread_mutex_unlock(&worker_mutex);
    if (pthread_create(&worker_thread, NULL, pair_worker, request)) { free(request); snprintf(arr_message, sizeof arr_message, "Cannot start pairing."); return; }
    b_busy = b_thread = true;
    snprintf(arr_message, sizeof arr_message, "Pairing securely with the host...");
}

static void cancel_pair(void)
{
    if (!b_busy) { b_visible = false; memset(arr_code, 0, sizeof arr_code); return; }
    pthread_mutex_lock(&worker_mutex); b_cancel = true; if (worker_pid > 0) kill(-worker_pid, SIGTERM); pthread_mutex_unlock(&worker_mutex);
    snprintf(arr_message, sizeof arr_message, "Cancelling pairing...");
}

static void append_character(unsigned int ch)
{
    if (!b_visible || b_busy || ch > 127) return;
    bool valid = int_focus == 0 ? (ch >= '0' && ch <= '9') || ch == '.' :
        (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!valid) return;
    char *field = int_focus == 0 ? arr_host : arr_code;
    size_t capacity = int_focus == 0 ? sizeof arr_host : sizeof arr_code;
    if (b_selected) { field[0] = 0; b_selected = false; }
    size_t length = strlen(field);
    if (length < capacity - 1) { field[length] = (char)ch; field[length + 1] = 0; }
    arr_message[0] = 0;
}

static void on_character(GLFWwindow *window, unsigned int ch) { (void)window; append_character(ch); }

void pairing_ui_key(GLFWwindow *window, int key, int action, int mods)
{
    if (!b_visible || (action != GLFW_PRESS && action != GLFW_REPEAT)) return;
    if (key == GLFW_KEY_ESCAPE) { b_escape = true; return; }
    if (b_busy) return;
    if (key == GLFW_KEY_TAB) { int_focus ^= 1; b_selected = false; }
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) b_enter = true;
    else if (key == GLFW_KEY_A && (mods & GLFW_MOD_CONTROL)) b_selected = true;
    else if ((key == GLFW_KEY_V && (mods & GLFW_MOD_CONTROL)) || (key == GLFW_KEY_INSERT && (mods & GLFW_MOD_SHIFT))) {
        const char *clipboard = glfwGetClipboardString(window);
        if (clipboard) for (size_t i = 0; clipboard[i] && i < 1024; i++) append_character((unsigned char)clipboard[i]);
    } else if (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE) {
        char *field = int_focus == 0 ? arr_host : arr_code;
        if (b_selected) field[0] = 0;
        else { size_t length = strlen(field); if (length) field[length - 1] = 0; }
        b_selected = false;
    }
}

void pairing_ui_init(GLFWwindow *window, bool legacy_ready)
{
    b_ready = b_ready || legacy_ready;
    const char *host = getenv("FUSE_SERVER_IP");
    if (host) snprintf(arr_host, sizeof arr_host, "%s", host);
    glfwSetCharCallback(window, on_character);
    if (!b_ready) pairing_ui_open();
}
bool pairing_ui_ready(void) { return b_ready; }
bool pairing_ui_visible(void) { return b_visible; }
const char *pairing_ui_host(void) { return arr_saved_host; }
void pairing_ui_open(void) { b_visible = true; b_enter = b_escape = false; arr_message[0] = 0; int_focus = *arr_host ? 1 : 0; }

void pairing_ui_poll(GLFWwindow *window)
{
    if (b_thread) {
        pthread_mutex_lock(&worker_mutex); bool done = b_done; pthread_mutex_unlock(&worker_mutex);
        if (done) {
            pthread_join(worker_thread, NULL); b_thread = b_busy = false;
            pthread_mutex_lock(&worker_mutex);
            snprintf(arr_message, sizeof arr_message, "%s", arr_worker_message);
            if (b_success && apply_profile(arr_profile)) b_visible = false;
            if (b_cancel) b_visible = false;
            pthread_mutex_unlock(&worker_mutex);
        }
    }
    double x, y; int width, height;
    glfwGetCursorPos(window, &x, &y); glfwGetWindowSize(window, &width, &height);
    if (width > 0 && height > 0) { x *= 800.0 / width; y *= 600.0 / height; }
    bool mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool clicked = mouse && !b_mouse; b_mouse = mouse;
    if (!b_visible) return;
    if (b_escape) { b_escape = false; cancel_pair(); }
    if (clicked && x >= 180 && x <= 620) {
        if (y >= 220 && y <= 264) { int_focus = 0; b_selected = false; }
        if (y >= 310 && y <= 354) { int_focus = 1; b_selected = false; }
        if (y >= 465 && y <= 513) { if (x >= 410) cancel_pair(); else if (!b_busy) start_pair(); }
    }
    if (b_enter) { b_enter = false; if (!b_busy) start_pair(); }
}

static void box(float x, float y, float w, float h, float r, float g, float b)
{
    glColor3f(r, g, b); glBegin(GL_QUADS); glVertex2f(x,y); glVertex2f(x+w,y);
    glVertex2f(x+w,y+h); glVertex2f(x,y+h); glEnd();
}
static void label(float x, float y, float scale, const char *value)
{ glColor3f(.91f,.94f,.98f); ui_draw_text(x,y,scale,value); }

void pairing_ui_draw(void)
{
    glDisable(GL_TEXTURE_2D);
    box(0,0,800,600,.035f,.055f,.09f); box(140,55,520,490,.075f,.105f,.16f);
    box(140,55,520,5,.25f,.85f,.60f);
    label(180,95,3.2f,"Pair this player"); label(180,150,1.4f,"Use the next code displayed by the host.");
    label(180,196,1.5f,"Host IP"); label(180,286,1.5f,"Pairing code");
    for (int i = 0; i < 2; i++) {
        float y = i == 0 ? 220 : 310;
        box(178,y-2,444,48,.12f,int_focus == i ? .55f : .22f,.38f);
        box(180,y,440,44,.035f,.055f,.09f);
    }
    label(192,235,1.7f,*arr_host ? arr_host : "192.168.1.50");
    char masked[49]; size_t length = strlen(arr_code); memset(masked,'*',length); masked[length] = 0;
    label(192,325,1.35f,length ? masked : "Paste the FUSE code (Ctrl+V)");
    label(180,373,1.2f,"Tab: next field    Ctrl+A: select all    Esc: cancel");
    char line[59]; size_t message_length = strlen(arr_message);
    for (int i = 0; i < 3 && (size_t)i * 58 < message_length; i++) {
        snprintf(line, sizeof line, "%.58s", arr_message + i * 58);
        for (size_t j = 0; line[j]; j++) if (line[j] == '\n' || line[j] == '\r') line[j] = ' ';
        label(180,401 + i * 17,1.2f,line);
    }
    box(180,465,210,48,.10f,.43f,.32f); box(410,465,210,48,.20f,.25f,.34f);
    label(194,483,1.5f,b_busy ? "Pairing..." : "Pair [Enter]"); label(424,483,1.5f,"Cancel [Esc]");
}

void pairing_ui_shutdown(void)
{
    if (b_thread) { cancel_pair(); pthread_join(worker_thread, NULL); b_thread = b_busy = false; }
    memset(arr_code,0,sizeof arr_code); memset(arr_profile,0,sizeof arr_profile);
}
