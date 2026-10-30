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
a DDS worker thread does all network I/O (net_client.c).

Options (for measurements, see bench/render.sh):
  --stress N           draw N extra animated sprites every frame
  --frames F           exit after F frames and print frame-time statistics
                       (vsync is switched on so every run renders the same way)
  --render MODE        how sprites are drawn (optimisation #16, see render.h):
                       immediate, float or short (default)
  --particles N        keep N particles alive (bursts at random places)
  --particle-layout L  particle code (#5-#7, see particles.h): aos, soa (default)
                       soa-intrin or soa-intrin-scan (hand-written AVX2)
  --particle-threads T threads for the particle update (default 1; more only
                       pays off with tens of thousands of particles, see
                       bench/results/opt-05-particle-threads)
  --input-send MODE    when input is sent (#13, see net_client.h): change
                       (default: on change + every 100 ms) or every-10ms
                       (while a key is held, as the baseline)
  --interp on|off      draw positions interpolated between snapshots (#15,
                       default on) or the newest snapshot as received
  --smooth-stats S     after S seconds print smoothness statistics of the
                       players' on-screen movement (interp.h) and quit

Environment: FUSE_CLIENT_WAIT=spin  the DDS worker polls as the baseline
                                    did instead of waiting (#2) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <GLFW/glfw3.h>

#include "config.h"
#include "timing.h"
#include "net_client.h"
#include "input.h"
#include "render.h"
#include "particles.h"
#include "dds_security.h"
#include "interp.h"

// qsort comparison for frame times
static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Print statistics of the CPU time spent in render_frame() per frame, in
microseconds, as one key=value line (parsed by bench/render.sh). Sorts the
array in place. */
static void print_frame_stats(uint64_t *a_arr_ns, int a_int_frames, int a_int_stress,
                              RenderMode a_mode, double a_d_wall_s, double a_d_elapsed_s)
{
    double d_sum = 0;
    for (int i = 0; i < a_int_frames; i++)
        d_sum += (double)a_arr_ns[i];
    qsort(a_arr_ns, (size_t)a_int_frames, sizeof(uint64_t), compare_u64);

    // fps: render loop only; elapsed_s: from the start of main() (lets
    // bench/render.sh work out how many frames fall in a perf counting window)
    printf("frames=%d stress=%d mode=%s fps=%.2f elapsed_s=%.3f render_us_mean=%.1f"
           " render_us_p50=%.1f render_us_p99=%.1f render_us_max=%.1f gl_renderer=\"%s\"\n",
           a_int_frames, a_int_stress, render_mode_name(a_mode),
           a_int_frames / a_d_wall_s, a_d_elapsed_s,
           d_sum / a_int_frames / 1000.0,
           a_arr_ns[a_int_frames / 2] / 1000.0,
           a_arr_ns[(int)(a_int_frames * 0.99)] / 1000.0,
           a_arr_ns[a_int_frames - 1] / 1000.0,
           (const char *)glGetString(GL_RENDERER));
}

// GLFW callback: the framebuffer changed size (display scaling applied after
// the window opened, or the window moved to a monitor with another scale)
static void on_framebuffer_size(GLFWwindow *a_window, int a_int_w, int a_int_h)
{
    (void)a_window;
    render_resize(a_int_w, a_int_h);
}

int main(int argc, char **argv)
{
    uint64_t u_main_ns = timing_now_ns();

    // Command-line options (see the top of this file)
    int int_stress = 0;
    int int_frames = 0;
    RenderMode render_mode = RENDER_BATCHED_SHORT;
    int int_particles = 0;
    ParticleLayout particle_layout = PARTICLES_SOA;
    int int_particle_threads = 1;
    double d_smooth_stats_s = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--stress") == 0 && i + 1 < argc)
            int_stress = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            int_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--render") == 0 && i + 1 < argc &&
                 render_mode_from_name(argv[i + 1]) >= 0)
            render_mode = (RenderMode)render_mode_from_name(argv[++i]);
        else if (strcmp(argv[i], "--particles") == 0 && i + 1 < argc)
            int_particles = atoi(argv[++i]);
        else if (strcmp(argv[i], "--particle-layout") == 0 && i + 1 < argc &&
                 particles_layout_from_name(argv[i + 1]) >= 0)
            particle_layout = (ParticleLayout)particles_layout_from_name(argv[++i]);
        else if (strcmp(argv[i], "--particle-threads") == 0 && i + 1 < argc)
            int_particle_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--input-send") == 0 && i + 1 < argc &&
                 (strcmp(argv[i + 1], "change") == 0 || strcmp(argv[i + 1], "every-10ms") == 0))
            b_input_on_change = strcmp(argv[++i], "change") == 0;
        else if (strcmp(argv[i], "--interp") == 0 && i + 1 < argc &&
                 (strcmp(argv[i + 1], "on") == 0 || strcmp(argv[i + 1], "off") == 0))
            b_interp = strcmp(argv[++i], "on") == 0;
        else if (strcmp(argv[i], "--smooth-stats") == 0 && i + 1 < argc)
        {
            d_smooth_stats_s = atof(argv[++i]);
            b_interp_stats = d_smooth_stats_s > 0;
        }
        else
        {
            fprintf(stderr, "Usage: %s [--stress N] [--frames F] [--render immediate|float|short]\n"
                            "          [--particles N] [--particle-layout aos|soa|soa-intrin|soa-intrin-scan] [--particle-threads T]\n"
                            "          [--input-send change|every-10ms] [--interp on|off] [--smooth-stats S]\n",
                    argv[0]);
            return 1;
        }
    }

    dds_security_prepare();
	if (!net_client_prepare())
		return EXIT_FAILURE;
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
    // Room for the stress particles plus explosions (explosions are not
    // spawned yet: they come with the server's snapshots)
    particles_init(particle_layout, int_particles + PARTICLES_SPARE, int_particle_threads,
                   12345u);   // fixed seed: every run spawns the same particles
    render_init(render_mode, int_stress);

    // Fill the real framebuffer, which is not WIDTH x HEIGHT on a scaled display
    int int_fb_w, int_fb_h;
    glfwGetFramebufferSize(window, &int_fb_w, &int_fb_h);
    render_resize(int_fb_w, int_fb_h);
    glfwSetFramebufferSizeCallback(window, on_framebuffer_size);

    // Measured run: fixed vsync so frame pacing is the same on every run,
    // and one render_frame() time per frame
    uint64_t *arr_frame_ns = NULL;
    if (int_frames > 0)
    {
        glfwSwapInterval(1);
        arr_frame_ns = malloc((size_t)int_frames * sizeof(uint64_t));
        if (arr_frame_ns == NULL)
            return 1;
    }
    int int_frame = 0;
    uint64_t u_start_ns = timing_now_ns();

    // Screenshots: F12 saves screenshot.png; FUSE_SCREENSHOT=<file> saves one
    // automatically after 2 s (for tests and slides)
    const char *str_auto_shot = getenv("FUSE_SCREENSHOT");
    // Security handshakes can outlast the original two-second capture delay.
    // Keep the existing default, but let acceptance demos wait for all joins.
    double d_screenshot_delay = 2.0;
    const char *str_shot_delay = getenv("FUSE_SCREENSHOT_DELAY");
    if (str_shot_delay != NULL)
    {
        char *p_end;
        d_screenshot_delay = strtod(str_shot_delay, &p_end);
        if (*str_shot_delay == '\0' || *p_end != '\0' || !isfinite(d_screenshot_delay) ||
            d_screenshot_delay < 0.0 || d_screenshot_delay > 60.0)
        {
            fprintf(stderr, "FUSE_SCREENSHOT_DELAY must be a number from 0 to 60 seconds\n");
            return 1;
        }
    }
    int int_f12_prev = GLFW_RELEASE;
    double d_prev_s = glfwGetTime();

    // Render loop: no frame cap; swap interval left at the driver default (as in the baseline)
    while (!glfwWindowShouldClose(window))
    {
        /* Process keyboard/window events */
        glfwPollEvents();
        input_poll(window);

        // Particles move by the real time since the last frame (at most 0.1 s,
        // so a stalled frame does not throw them across the window)
        double d_now_s = glfwGetTime();
        double d_dt_s = d_now_s - d_prev_s;
        d_prev_s = d_now_s;
        particles_update((float)(d_dt_s < 0.1 ? d_dt_s : 0.1));
        if (int_particles > 0)
            particles_fill(int_particles);

        uint64_t u_t0 = timing_now_ns();
        render_frame();
        if (arr_frame_ns != NULL)
        {
            arr_frame_ns[int_frame++] = timing_now_ns() - u_t0;
            if (int_frame == int_frames)
            {
                uint64_t u_end_ns = timing_now_ns();
                print_frame_stats(arr_frame_ns, int_frames, int_stress, render_mode,
                                  (u_end_ns - u_start_ns) / 1e9, (u_end_ns - u_main_ns) / 1e9);
                break;
            }
        }

        int int_f12 = glfwGetKey(window, GLFW_KEY_F12);
        if (int_f12 == GLFW_PRESS && int_f12_prev == GLFW_RELEASE)
            render_save_screenshot("screenshot.png");
        int_f12_prev = int_f12;
        if (str_auto_shot != NULL && glfwGetTime() > d_screenshot_delay)
        {
            render_save_screenshot(str_auto_shot);
            str_auto_shot = NULL;
        }

        // --smooth-stats: print and quit (closing sends a Leave as usual)
        if (b_interp_stats && glfwGetTime() > d_smooth_stats_s)
        {
            interp_stats_print();
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        glfwSwapBuffers(window);
    }

    // Leave the game (the worker sends a Leave and closes DDS)
    net_client_stop();
    pthread_join(thread, NULL);

    particles_free();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
