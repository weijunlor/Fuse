/* server/collision.c
AABB tests and spatial grid for embers. */

#include "collision.h"
#include "config.h"

bool collision_detect(Player a_arr_players[], int a_int_arr_len,
                      int a_int_id, int a_int_x, int a_int_y)
{
	bool b_res = false;
	// Loop through the array
	for(int i = 0; i < a_int_arr_len; i++)
	{
		// Index + 1 = Player ID
		// If a player other than the player itself is active
		if(i != a_int_id -1 && a_arr_players[i].b_active == true)
		{
			// If they overlap horizontally
			if(a_int_x <= a_arr_players[i].x + SQ_WIDTH &&
				a_arr_players[i].x <= a_int_x + SQ_WIDTH)
			{
				// If they overlap vertically
				if(a_int_y <= a_arr_players[i].y + SQ_WIDTH &&
					a_arr_players[i].y <= a_int_y + SQ_WIDTH)
				{
					b_res = true;
					break;
				}
			}
		}
	}

	return b_res;
}
