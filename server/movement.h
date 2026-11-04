/** @file server/movement.h
 * @brief Movement helpers shared by the game and optimisation #9 benchmark.
 * @details Arena coordinates are small integers exactly representable as floats.
 */
#ifndef SERVER_MOVEMENT_H
#define SERVER_MOVEMENT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"

/** @brief Clamp a pixel coordinate to the inclusive arena range.
 * @param a_int_value Coordinate to clamp.
 * @param a_int_max Nonnegative upper bound.
 * @param a_b_branchless True uses fminf/fmaxf; false uses conditional checks.
 * @return Coordinate within 0..a_int_max.
 */
static inline int movement_clamp(int a_int_value, int a_int_max,
                                bool a_b_branchless)
{
	if (a_b_branchless)
		return (int)fminf(fmaxf((float)a_int_value, 0.0f), (float)a_int_max);
	return a_int_value < 0 ? 0 :
	       (a_int_value > a_int_max ? a_int_max : a_int_value);
}

/** @brief Convert held movement keys into horizontal and vertical directions.
 * @param a_u8_keys KEY_* bitmask; dash does not affect direction.
 * @param[out] a_p_dx Horizontal direction: -1, 0 or 1.
 * @param[out] a_p_dy Vertical direction: -1, 0 or 1.
 * @param a_b_lookup True uses lookup tables; false uses conditional checks.
 * @note Opposite keys cancel; diagonal direction is not normalised.
 */
static inline void movement_direction(uint8_t a_u8_keys, int *a_p_dx,
                                      int *a_p_dy, bool a_b_lookup)
{
	static const int8_t arr_dx[16] =
		{0, 0, 0, 0, -1, -1, -1, -1, 1, 1, 1, 1, 0, 0, 0, 0};
	static const int8_t arr_dy[16] =
		{0, -1, 1, 0, 0, -1, 1, 0, 0, -1, 1, 0, 0, -1, 1, 0};
	if (a_b_lookup)
	{
		*a_p_dx = arr_dx[a_u8_keys & 15];
		*a_p_dy = arr_dy[a_u8_keys & 15];
	}
	else
	{
		*a_p_dx = 0;
		*a_p_dy = 0;
		if (a_u8_keys & KEY_LEFT) (*a_p_dx)--;
		if (a_u8_keys & KEY_RIGHT) (*a_p_dx)++;
		if (a_u8_keys & KEY_UP) (*a_p_dy)--;
		if (a_u8_keys & KEY_DOWN) (*a_p_dy)++;
	}
}

#endif /* SERVER_MOVEMENT_H */
