/* client/net_client.h
Client DDS worker thread: sends input, receives snapshots. */

#ifndef CLIENT_NET_CLIENT_H
#define CLIENT_NET_CLIENT_H

#include <pthread.h>
#include <stdbool.h>

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

// #13: true (default) sends an Input only when the held keys change, plus a
// repeat every 100 ms; false sends one every 10 ms while keys are held (the
// baseline rate). Set before starting the worker (--input-send in main.c).
extern bool b_input_on_change;

/* DDS worker thread (start with pthread_create). Polls in a loop: sends a
join request every 1 s until joined, then sends the held keys (see
b_input_on_change), and copies each received GameSnapshot into arr_Players.
Runs until net_client_stop() is called. */
void *net_client_worker(void *arg);

/* Ask the worker to stop: it sends a Leave (if joined, and FUSE_LEAVE is not
off), waits up to 0.5 s for the server to acknowledge it, closes DDS and
returns. Called by the main thread when the window closes; pthread_join the
worker afterwards. (The baseline exited without telling the server.) */
void net_client_stop(void);

#endif /* CLIENT_NET_CLIENT_H */
