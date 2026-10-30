/* Movement hot paths shared by the game and the #9 benchmark.
 * Coordinates are small integers (0..800), exactly representable as floats. */
#ifndef SERVER_MOVEMENT_H
#define SERVER_MOVEMENT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"

static inline int movement_clamp(int a_int_value, int a_int_max,
                                bool a_b_branchless)
{
	if (a_b_branchless)
		return (int)fminf(fmaxf((float)a_int_value, 0.0f), (float)a_int_max);
	return a_int_value < 0 ? 0 :
	       (a_int_value > a_int_max ? a_int_max : a_int_value);
}

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
