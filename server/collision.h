/** @file server/collision.h
 * @brief Square collision tests and server-only ember simulation (#8/#9).
 * @details Ember queries support a spatial grid and a brute-force comparison.
 * Pool functions do not lock: use them on the simulation thread under the
 * game mutex, or serially in a benchmark before starting other threads.
 */

#ifndef SERVER_COLLISION_H
#define SERVER_COLLISION_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"
#include "config.h"

#define MAX_EMBERS 4096
#define EMBER_LIFETIME_TICKS (8 * TICK_HZ)

/** @brief Test two axis-aligned squares for overlap (#9).
 * @param a_int_x First square's left coordinate in pixels.
 * @param a_int_y First square's top coordinate in pixels.
 * @param a_int_size First square's side length.
 * @param a_int_other_x Second square's left coordinate.
 * @param a_int_other_y Second square's top coordinate.
 * @param a_int_other_size Second square's side length.
 * @param a_b_inclusive True counts touching edges; false requires overlap.
 * @param a_b_branchless True combines comparisons with bitwise AND.
 * @return True when the squares meet the selected overlap rule.
 */
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

/** @brief One slot in the fixed-array ember comparison layout.
 * @details The ring/SoA layout is a later team optimisation (#8b).
 */
typedef struct
{
	int x;
	int y;
	uint16_t u16_age_ticks;
	bool b_active;
} Ember;

/** @brief Check a proposed player position against other active players.
 * @param a_arr_players Player table; the moving player's own slot is skipped.
 * @param a_int_arr_len Number of entries in the table.
 * @param a_int_id Moving player's 1-based ID.
 * @param a_int_x Proposed left coordinate in pixels.
 * @param a_int_y Proposed top coordinate in pixels.
 * @return True when another active square blocks the position (edges included).
 * @pre The caller holds the game mutex when using shared player state.
 */
bool collision_detect(Player a_arr_players[], int a_int_arr_len,
                      int a_int_id, int a_int_x, int a_int_y);

/** @brief Empty both ember layouts and clear the grid index. */
void collision_embers_clear(void);

/** @brief Spawn up to 32 embers around an explosion, dropping excess if full.
 * @param a_int_x Explosion centre x coordinate in pixels.
 * @param a_int_y Explosion centre y coordinate in pixels.
 * @param a_u16_seed Seed for repeatable offsets in the range -60..60 pixels.
 */
void collision_embers_spawn(int a_int_x, int a_int_y, uint16_t a_u16_seed);

/** @brief Advance ember ages by one tick and expire old entries. */
void collision_embers_tick(void);

/** @brief Query nearby grid cells for an ember touching a player square.
 * @param a_int_x Player's left coordinate in pixels.
 * @param a_int_y Player's top coordinate in pixels.
 * @return True on strict overlap; touching edges alone do not count.
 * @note Lazily rebuilds the grid after a spawn or expiry.
 */
bool collision_player_hits_ember(int a_int_x, int a_int_y);

/** @brief Query all live embers without the spatial grid (comparison path).
 * @param a_int_x Player's left coordinate in pixels.
 * @param a_int_y Player's top coordinate in pixels.
 * @return The same overlap result as collision_player_hits_ember().
 */
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
