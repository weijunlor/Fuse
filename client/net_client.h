/* client/net_client.h
Client DDS worker thread: sends input, receives snapshots. */

#ifndef CLIENT_NET_CLIENT_H
#define CLIENT_NET_CLIENT_H

#include <pthread.h>

#include "config.h"
#include "types.h"

// This client's player ID from the server; -1 until joined.
// Written by the worker under the mutex, but read without it by the worker
// and the render code (baseline data race, optimisation #4).
extern int int_player_id;

// Mirror of the server's player table, filled from GameSnapshot messages
extern Player arr_Players[MAX_PLAYERS];

// One mutex guards int_player_id, arr_Players and u8_keys (input.h)
extern pthread_mutex_t mutex;

/* DDS worker thread (start with pthread_create, never returns). Polls in a
loop: sends a join request every 1 s until joined, then sends the held keys
every 10 ms while any key is held (and once on release), and copies each
received GameSnapshot into arr_Players. */
void *net_client_worker(void *arg);

#endif /* CLIENT_NET_CLIENT_H */
