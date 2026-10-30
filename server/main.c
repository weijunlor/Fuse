/* server/main.c
FUSE server entry point. Based on baseline/server.c (Dr Khoo Teck Ping,
27 Aug 2026), extended with a fixed tick, game rules and optional AI bots:

1. DDS server (net_server.c)
2. Supports max 5 clients
3. Start this server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys (game.c)
6. Collision detection among all players and the window is supported (collision.c)

Threads: the main thread admits joining players (lobby.c) and runs the game
at a fixed TICK_HZ ticks per second (game.c); a DDS worker thread does all
network I/O (net_server.c).

Options:
  --snapshot-hz H   publish a GameSnapshot H times per second (default
                    SNAPSHOT_HZ; H must divide TICK_HZ: 60, 30, 20, 15, 12,
                    10, ...). Lower rates need client interpolation (#15).
  --player-timeout S  remove a player whose client sent no Input for S seconds
                    (default PLAYER_TIMEOUT_S = 5; 0 = never). FUSE_LEAVE=off
                    in the environment turns Leave and the timeout off.
  --bots N         reserve N slots for chase/flee bots (0..4, default 0).
  --wait, --publish, --collision, --branches, --reduction retain before paths
                    for isolated profiling; see README.md. */

Environment (to measure optimisations against the old behaviour):
  FUSE_SERVER_WAIT=spin      busy-poll in both threads as before #2
  FUSE_WRITE_UNDER_LOCK=on   publish snapshots while holding the mutex (#1) */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#include "net_server.h"
#include "lobby.h"
#include "game.h"
#include "collision.h"
#include "timing.h"
#include "log.h"

// Nanoseconds per tick
#define TICK_NS (1000000000ull / TICK_HZ)

static bool parse_number(const char *a_str_value, int *a_p_out)
{
	char *str_end;
	errno = 0;
	long l_value = strtol(a_str_value, &str_end, 10);
	if (errno || str_end == a_str_value || *str_end || l_value < 0 || l_value > INT_MAX)
		return false;
	*a_p_out = (int)l_value;
	return true;
}

int main(int argc, char **argv)
{
	int int_bots = 0;
	// Command-line options (see the top of this file)
	for (int i = 1; i < argc; i++)
	{
		bool b_ok = false;
		if (strcmp(argv[i], "--snapshot-hz") == 0 && i + 1 < argc)
		{
			int int_hz;
			b_ok = parse_number(argv[++i], &int_hz) &&
			       int_hz >= 1 && int_hz <= TICK_HZ && TICK_HZ % int_hz == 0;
			if (b_ok)
				int_snapshot_every = TICK_HZ / int_hz;
		}
		else if (strcmp(argv[i], "--player-timeout") == 0 && i + 1 < argc)
		{
			b_ok = parse_number(argv[++i], &int_player_timeout_s);
		}
		else if (strcmp(argv[i], "--bots") == 0 && i + 1 < argc)
		{
			b_ok = parse_number(argv[++i], &int_bots) && int_bots < MAX_PLAYERS;
		}
		else if (i + 1 < argc)
		{
			const char *str_option = argv[i];
			const char *str_value = argv[++i];
			if (strcmp(str_option, "--wait") == 0)
			{
				b_ok = strcmp(str_value, "sleep") == 0 || strcmp(str_value, "busy") == 0;
				b_server_wait = strcmp(str_value, "sleep") == 0;
			}
			else if (strcmp(str_option, "--publish") == 0)
			{
				b_ok = strcmp(str_value, "unlocked") == 0 || strcmp(str_value, "locked") == 0;
				b_publish_locked = strcmp(str_value, "locked") == 0;
			}
			else if (strcmp(str_option, "--collision") == 0)
			{
				b_ok = strcmp(str_value, "grid") == 0 || strcmp(str_value, "brute") == 0;
				b_ember_grid = strcmp(str_value, "grid") == 0;
			}
			else if (strcmp(str_option, "--branches") == 0)
			{
				b_ok = strcmp(str_value, "branchless") == 0 || strcmp(str_value, "branchy") == 0;
				b_game_branchless = b_collision_branchless = strcmp(str_value, "branchless") == 0;
			}
			else if (strcmp(str_option, "--reduction") == 0)
			{
				b_ok = strcmp(str_value, "accum") == 0 || strcmp(str_value, "serial") == 0;
				b_reduce_accum = strcmp(str_value, "accum") == 0;
			}
		}
		if (!b_ok)
		{
			fprintf(stderr, "Usage: %s [--snapshot-hz H] [--player-timeout S] [--bots N]\n"
			                "  [--wait sleep|busy] [--publish unlocked|locked]\n"
			                "  [--collision grid|brute] [--branches branchless|branchy]\n"
			                "  [--reduction accum|serial]\n"
			                "  H divides %d; S >= 0 (0 = never); bots 0..%d\n",
			        argv[0], TICK_HZ, MAX_PLAYERS - 1);
			return 1;
		}
	}

	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));
	if (!lobby_add_bots(int_bots))
	{
		LOG_ERROR("Could not create bots\n");
		return 1;
	}

	// Read the #1/#2 switches before the worker starts
	net_server_init();

	pthread_t thread;

	if (pthread_create(&thread, NULL, net_server_worker, NULL) != 0)
	{
		perror("pthread_create");
		return 1;
	}

	LOG_INFO("Waiting... (%d ticks/s, a snapshot every %d ticks)\n",
	         TICK_HZ, int_snapshot_every);

	/* Ticks are scheduled on absolute times (start + n * TICK_NS), so the
	rate does not drift. #2: sleep until the next tick instead of spinning
	(FUSE_SERVER_WAIT=spin keeps the busy loop). Join requests and Leaves
	queued by the worker are therefore handled once per tick, at most 1/TICK_HZ
	s after they arrive. */
	uint64_t u_next_tick_ns = timing_now_ns();
	while (1)
	{
		lobby_poll();

		uint64_t u_now_ns = timing_now_ns();
		if (u_now_ns < u_next_tick_ns)
		{
			if (!b_server_wait)
				continue;   // spin: poll the lobby and the clock again
			uint64_t u_sleep_ns = u_next_tick_ns - u_now_ns;
			struct timespec ts_sleep = {
				.tv_sec = (time_t)(u_sleep_ns / 1000000000ull),
				.tv_nsec = (long)(u_sleep_ns % 1000000000ull)
			};
			if (b_server_wait)
				nanosleep(&ts_sleep, NULL);
			continue;
		}

		// Remove players not heard from for too long, then move the rest
		lobby_check_timeouts(u_now_ns);
		game_tick();
		// The worker sleeps in its waitset: wake it to publish the new
		// snapshot and any join answers lobby_poll() made above
		net_server_wake();
		u_next_tick_ns += TICK_NS;
		// More than a second behind (e.g. the process was stopped):
		// skip the missed ticks instead of running them all at once
		if (u_now_ns > u_next_tick_ns + 1000000000ull)
			u_next_tick_ns = u_now_ns + TICK_NS;
	}

	return 0;
}
