/* server/game.h
Server simulation tick: movement, bomb/fuse, rounds, pickups, win state. */

#ifndef SERVER_GAME_H
#define SERVER_GAME_H

#include <pthread.h>

#include "config.h"
#include "types.h"

// Table to track clients. Player ID is the array index plus 1.
extern Player arr_Players[MAX_PLAYERS];

// One mutex guards the player table and the lobby flags (lobby.h),
// shared by the main thread and the DDS worker thread
extern pthread_mutex_t mutex;

// Move a player one STEP in direction 'u', 'd', 'l' or 'r', unless the move
// leaves the window or collides with another player. Locks the mutex itself.
// The player ID is not range-checked (as in the baseline).
void game_apply_input(int a_int_player_id, char a_ch_dir);

#endif /* SERVER_GAME_H */
