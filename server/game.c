/* server/game.c
Server simulation tick: movement, bomb/fuse, rounds, pickups, win state. */

#include "game.h"
#include "collision.h"

// Init all members to 0
Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void game_apply_input(int a_int_player_id, char a_ch_dir)
{
	// Get the player's current coords
	pthread_mutex_lock(&mutex);
	// Index is player ID minus 1
	int int_x = arr_Players[a_int_player_id-1].x;
	int int_y = arr_Players[a_int_player_id-1].y;
	pthread_mutex_unlock(&mutex);

	switch (a_ch_dir)
	{
		case 'u':
			int_y = int_y - STEP;
			break;
		case 'd':
			int_y = int_y + STEP;
			break;
		case 'l':
			int_x = int_x - STEP;
			break;
		case 'r':
			int_x = int_x + STEP;
			break;

		default:
			break;
	}

	// Check collision with window
	if(int_x >= 0 && int_x <= WIDTH-SQ_WIDTH)
	{
		if(int_y >= 0 && int_y <= HEIGHT-SQ_WIDTH)
		{
			// Check collision with other players
			pthread_mutex_lock(&mutex);
			bool b_res = collision_detect(arr_Players,
				MAX_PLAYERS, a_int_player_id, int_x, int_y);
			pthread_mutex_unlock(&mutex);
			// If no collision detected
			if(b_res == false)
			{
				//Update that player's position
				pthread_mutex_lock(&mutex);
				arr_Players[a_int_player_id-1].x = int_x;
				arr_Players[a_int_player_id-1].y = int_y;
				pthread_mutex_unlock(&mutex);
			}
		}
	}
}
