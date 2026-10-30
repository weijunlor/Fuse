/* server/game.h
Server simulation tick: movement, bomb/fuse, rounds, pickups, win state. */

#ifndef SERVER_GAME_H
#define SERVER_GAME_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "types.h"
#include "fuse.h"

// Table to track clients. Player ID is the array index plus 1.
extern Player arr_Players[MAX_PLAYERS];

// State mutex: shared with DDS for mutex handoff, or the optional locked-write
// comparison. Default SPSC publication does not acquire it.
// In SPSC mode the main thread owns players/keys/auth/lobby state, and the
// worker sees copies through rings and a pinned snapshot buffer.
extern pthread_mutex_t mutex;

// Snapshots are published every this many ticks (TICK_HZ / --snapshot-hz)
extern int int_snapshot_every;

// Store the keys (KEY_* bitmask, types.h) that a player holds now, from an
// Input message; the next tick moves the player by them. Also records when
// the player was last heard from (for the timeout in lobby.c). IDs outside
// 1..MAX_PLAYERS are ignored. Locks the mutex itself. Called by the DDS worker.
void game_set_keys(int a_int_player_id, uint8_t a_u8_keys);

// Same update with the mutex already held (authentication + update is atomic).
void game_set_keys_locked(int a_int_player_id, uint8_t a_u8_keys);

// Start a slot afresh for a player who joins or leaves: no keys held, last
// heard from now. The caller holds the mutex.
void game_reset_player(int a_int_index);

// When the player at a_int_index last sent an Input (or joined), in
// timing_now_ns() nanoseconds. The caller holds the mutex.
uint64_t game_last_input_ns(int a_int_index);

// Advance the game by one fixed step of 1/TICK_HZ s: move every active player
// by its held keys (PLAYER_SPEED, stopping at the window edge and at other
// players), then build the GameSnapshot for this tick. Locks the mutex itself.
// Called by the main thread TICK_HZ times per second.
void game_tick(void);

// Copy the latest snapshot to *a_p_out if one is waiting to be published,
// and mark it taken. Returns false if there is nothing new.
// SPSC: DDS worker copies from a pinned double buffer without taking mutex.
// Mutex comparison: the caller must hold mutex, as in the original path.
bool game_take_snapshot(GameSnapshot *a_p_out);

#endif /* SERVER_GAME_H */
