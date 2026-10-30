/* client/net_client.h
Client DDS worker thread: sends input, receives snapshots. */

#ifndef CLIENT_NET_CLIENT_H
#define CLIENT_NET_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "types.h"
#include "fuse.h"

// Worker-owned ID and snapshot are published TOGETHER. The renderer never
// accesses the worker's live state (#4), and uses one complete copy per frame.
typedef struct
{
    int int_player_id; // -1 until joined
    uint64_t u64_received_ns; // snapshot arrival time for render-thread interpolation
    GameSnapshot snapshot;
} ClientFrame;

// Main thread only. False means no new frame (or a publication collision);
// leave the caller's previous frame unchanged. Initialise its ID to -1.
bool net_client_read_frame(ClientFrame *a_frame);

// Main -> DDS worker: an atomic held-key value, latest state wins.
void net_client_set_keys(uint8_t a_u8_keys);

// #13: true (default) sends an Input only when the held keys change, plus a
// repeat every 100 ms; false sends one every 10 ms while keys are held (the
// baseline rate). Set before starting the worker (--input-send in main.c).
extern bool b_input_on_change;

// #2: waitset by default, or FUSE_CLIENT_WAIT=spin for comparison.
extern bool b_client_wait;
void net_client_init(void);
void net_client_wake(void);

// Prepare login, frame handoff and the waitset guard before starting the worker.
bool net_client_prepare(void);

/* DDS worker sends authenticated inputs and publishes copied ClientFrames.
Waits for DDS, changed keys, shutdown or the next timed message.
Runs until net_client_stop() is called. */
void *net_client_worker(void *arg);

/* Ask the worker to stop: it sends a Leave (if joined, and FUSE_LEAVE is not
off), waits up to 0.5 s for the server to acknowledge it, closes DDS and
returns. Called by the main thread when the window closes; pthread_join the
worker afterwards. (The baseline exited without telling the server.) */
void net_client_stop(void);

#endif /* CLIENT_NET_CLIENT_H */
