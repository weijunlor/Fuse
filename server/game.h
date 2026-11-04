/**
 * @file server/game.h
 * @brief Server simulation API for movement, rounds, lives and snapshots.
 * @details Player IDs are 1..MAX_PLAYERS; slot indices are 0..MAX_PLAYERS-1.
 * The main thread owns simulation state in SPSC mode. Shared-state operations
 * use the game mutex; individual functions describe who acquires it.
 */

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

// Before/after paths, selected by main before the worker starts.
extern bool b_game_branchless;
extern bool b_ember_grid;
extern bool b_reduce_accum;

/** @brief Check whether the current phase permits new players.
 * @pre The caller holds mutex.
 * @return True in PHASE_LOBBY; false during a match.
 */
bool game_accepts_joins(void);

/** @brief Store a human player's held keys and refresh its heartbeat.
 * @param a_int_player_id Player ID in 1..MAX_PLAYERS; invalid IDs are ignored.
 * @param a_u8_keys KEY_* bitmask; unsupported bits are discarded.
 * @note Acquires mutex. Inactive slots and bot slots are ignored.
 */
void game_set_keys(int a_int_player_id, uint8_t a_u8_keys);

/** @brief Apply the same key/heartbeat update without acquiring mutex.
 * @param a_int_player_id Player ID in 1..MAX_PLAYERS.
 * @param a_u8_keys Current KEY_* bitmask.
 * @pre The caller holds mutex and has validated the network message.
 * @see game_set_keys
 */
void game_set_keys_locked(int a_int_player_id, uint8_t a_u8_keys);

/** @brief Reset keys, lives, temporary effects and heartbeat for a slot.
 * @param a_int_index Slot index in 0..MAX_PLAYERS-1.
 * @pre The caller holds mutex. Position and active status are managed separately.
 */
void game_reset_player(int a_int_index);

/** @brief Read the last accepted input or reset time for a slot.
 * @param a_int_index Slot index in 0..MAX_PLAYERS-1.
 * @pre The caller holds mutex.
 * @return Monotonic timestamp in timing_now_ns() nanoseconds.
 */
uint64_t game_last_input_ns(int a_int_index);

/** @brief Advance one fixed simulation step and prepare its snapshot.
 * @details Updates ember expiry, phase timers, bots, movement, pickups and
 * bomb passing. Publishes a snapshot at the configured tick divisor.
 * @note Main thread only, TICK_HZ calls per second. Acquires mutex internally.
 */
void game_tick(void);

/** @brief Consume the latest completed snapshot for the DDS worker.
 * @param[out] a_p_out Receives a snapshot on success; otherwise unchanged.
 * @return True when a snapshot was copied; false if none is available or the
 * SPSC read collides with publication.
 * @note One consumer only. In mutex mode the caller must hold mutex;
 * the SPSC path uses a pinned buffer without that lock.
 */
bool game_take_snapshot(GameSnapshot *a_p_out);

#endif /* SERVER_GAME_H */
