/* server/collision.h
AABB tests and spatial grid for embers. */

#ifndef SERVER_COLLISION_H
#define SERVER_COLLISION_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"
#include "config.h"

#define MAX_EMBERS 4096
#define EMBER_LIFETIME_TICKS (8 * TICK_HZ)

/* #9: all four comparisons are independent in the optimised path. Keep
 * strict edges for embers/pickups and inclusive edges for player blocking. */
static inline bool collision_overlap(int a_int_x, int a_int_y, int a_int_size,
                                     int a_int_other_x, int a_int_other_y,
                                     int a_int_other_size, bool a_b_inclusive,
                                     bool a_b_branchless)
{
	int int_edge = a_b_inclusive ? 0 : 1;
	bool b_left = a_int_x <= a_int_other_x + a_int_other_size - int_edge;
	bool b_right = a_int_other_x <= a_int_x + a_int_size - int_edge;
	bool b_top = a_int_y <= a_int_other_y + a_int_other_size - int_edge;
	bool b_bottom = a_int_other_y <= a_int_y + a_int_size - int_edge;
	if (a_b_branchless)
		return b_left & b_right & b_top & b_bottom;
	return b_left && b_right && b_top && b_bottom;
}

// One slot of the "array" ember layout (the before for #8b): 4096 of these
// (12 B each, 48 KB) whether or not they are in use.
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

// Selected before threads start; benchmarks can compare the same workload.
extern bool b_collision_branchless;

// #8b ember storage, selected before any ember is spawned:
// true  (--embers ring, default): live embers only, in a FIFO ring of
//       separate x / y / spawn-tick arrays (SoA); see collision.c.
// false (--embers array): the fixed 4096-slot array of Ember structs that
//       every tick, spawn and grid rebuild scans in full.
// Collision results are identical in both layouts.
extern bool b_ember_ring;

#endif /* SERVER_COLLISION_H */
