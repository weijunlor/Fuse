/* server/collision.h
AABB tests and spatial grid for embers. */

#ifndef SERVER_COLLISION_H
#define SERVER_COLLISION_H

#include <stdbool.h>

#include "types.h"

// Check if the coords collide with other squares.
// Does not lock the mutex: the caller must hold it.
bool collision_detect(Player a_arr_players[], int a_int_arr_len,
                      int a_int_id, int a_int_x, int a_int_y);

#endif /* SERVER_COLLISION_H */
