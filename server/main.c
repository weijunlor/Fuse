/* server/main.c
FUSE server entry point. Based on baseline/server.c (Dr Khoo Teck Ping,
27 Aug 2026), split into modules with behaviour unchanged:

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
                    in the environment turns Leave and the timeout off. */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "net_server.h"
#include "lobby.h"
#include "game.h"
#include "timing.h"
#include "log.h"

// Nanoseconds per tick
#define TICK_NS (1000000000ull / TICK_HZ)

int main(int argc, char **argv)
{
	// Command-line options (see the top of this file)
	for (int i = 1; i < argc; i++)
	{
		bool b_ok = false;
		if (strcmp(argv[i], "--snapshot-hz") == 0 && i + 1 < argc)
		{
			int int_hz = atoi(argv[++i]);
			b_ok = int_hz >= 1 && int_hz <= TICK_HZ && TICK_HZ % int_hz == 0;
			if (b_ok)
				int_snapshot_every = TICK_HZ / int_hz;
		}
		else if (strcmp(argv[i], "--player-timeout") == 0 && i + 1 < argc)
		{
			int_player_timeout_s = atoi(argv[++i]);
			b_ok = int_player_timeout_s >= 0;
		}
		if (!b_ok)
		{
			fprintf(stderr, "Usage: %s [--snapshot-hz H] [--player-timeout S]\n"
			                "  H divides %d; S in seconds, 0 = never\n", argv[0], TICK_HZ);
			return 1;
		}
	}

	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));

	pthread_t thread;

	if (pthread_create(&thread, NULL, net_server_worker, NULL) != 0)
	{
		perror("pthread_create");
		return 1;
	}

	LOG_INFO("Waiting... (%d ticks/s, a snapshot every %d ticks)\n",
	         TICK_HZ, int_snapshot_every);

	/* Ticks are scheduled on absolute times (start + n * TICK_NS), so the
	rate does not drift. Sleep until the next tick instead of spinning. */
	uint64_t u_next_tick_ns = timing_now_ns();
	while (1)
	{
		lobby_poll();

		uint64_t u_now_ns = timing_now_ns();
		if (u_now_ns < u_next_tick_ns)
		{
			uint64_t u_sleep_ns = u_next_tick_ns - u_now_ns;
			struct timespec ts_sleep = {
				.tv_sec = (time_t)(u_sleep_ns / 1000000000ull),
				.tv_nsec = (long)(u_sleep_ns % 1000000000ull)
			};
			nanosleep(&ts_sleep, NULL);
			continue;
		}

		// Remove players not heard from for too long, then move the rest
		lobby_check_timeouts(u_now_ns);
		game_tick();
		u_next_tick_ns += TICK_NS;
		// More than a second behind (e.g. the process was stopped):
		// skip the missed ticks instead of running them all at once
		if (u_now_ns > u_next_tick_ns + 1000000000ull)
			u_next_tick_ns = u_now_ns + TICK_NS;
	}

	return 0;
}
