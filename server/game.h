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

// One mutex guards the player table, the held keys, the latest snapshot and
// the lobby flags (lobby.h), shared by the main thread and the DDS worker thread
extern pthread_mutex_t mutex;

// Snapshots are published every this many ticks (TICK_HZ / --snapshot-hz)
extern int int_snapshot_every;

// Store the keys (KEY_* bitmask, types.h) that a player holds now, from an
// Input message; the next tick moves the player by them. IDs outside
// 1..MAX_PLAYERS are ignored. Locks the mutex itself. Called by the DDS worker.
void game_set_keys(int a_int_player_id, uint8_t a_u8_keys);

// Advance the game by one fixed step of 1/TICK_HZ s: move every active player
// by its held keys (PLAYER_SPEED, stopping at the window edge and at other
// players), then build the GameSnapshot for this tick. Locks the mutex itself.
// Called by the main thread TICK_HZ times per second.
void game_tick(void);

// Copy the latest snapshot to *a_p_out if one is waiting to be published,
// and mark it taken. Returns false if there is nothing new.
// The caller must hold the mutex.
bool game_take_snapshot(GameSnapshot *a_p_out);

#endif /* SERVER_GAME_H */
