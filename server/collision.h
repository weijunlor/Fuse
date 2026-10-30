/* server/collision.h
AABB tests and spatial grid for embers. */

#ifndef SERVER_COLLISION_H
#define SERVER_COLLISION_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

#define MAX_EMBERS 4096

typedef struct
{
	int x;
	int y;
	uint16_t u16_age_ticks;
	bool b_active;
} Ember;

// Check if the coords collide with other squares.
// Does not lock the mutex: the caller must hold it.
bool collision_detect(Player a_arr_players[], int a_int_arr_len,
                      int a_int_id, int a_int_x, int a_int_y);

// Server-only ember pool. The client recreates visual embers from Explosion
// records, so these simulation details are deliberately not in the snapshot.
void collision_embers_clear(void);
void collision_embers_spawn(int a_int_x, int a_int_y, uint16_t a_u16_seed);
void collision_embers_tick(void);
bool collision_player_hits_ember(int a_int_x, int a_int_y);
bool collision_player_hits_ember_bruteforce(int a_int_x, int a_int_y);

#endif /* SERVER_COLLISION_H */
