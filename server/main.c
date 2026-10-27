/* server/main.c
FUSE server entry point. Based on baseline/server.c (Dr Khoo Teck Ping,
27 Aug 2026), split into modules with behaviour unchanged:

1. DDS server (net_server.c)
2. Supports max 5 clients
3. Start this server, then start client one by one
4. Each client can see itself and all other clients
5. Each client can move itself using the arrow keys (game.c)
6. Collision detection among all players and the window is supported (collision.c)

Threads: the main thread admits joining players (lobby.c); a DDS worker
thread does all network I/O (net_server.c). */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "net_server.h"
#include "lobby.h"

int main(void)
{
	// Seed rand(), so that the random numbers are different for each run
	srand((unsigned int)time(NULL));

	pthread_t thread;

	if (pthread_create(&thread, NULL, net_server_worker, NULL) != 0)
	{
		perror("pthread_create");
		return 1;
	}

	printf("Waiting...\n");
	// Busy loop: handle join requests as soon as the worker flags them
	while (1)
	{
		lobby_poll();
	}

	return 0;
}
