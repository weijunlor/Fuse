/* server/lobby.c
Player slots: join (nonce), leave, liveliness timeout. */

#include <stdbool.h>
#include <stdlib.h>

#include "lobby.h"
#include "game.h"
#include "collision.h"

// Only written by the main thread
static int int_num_active_players = 0;

// Flag set by worker thread that a player just joined
// Must be reset to false after processing by main thread
static bool b_player_join = false;

// Player ID is the array index plus 1
// -1 = No new players
// 0 = Invalid player ID (unable to admit new players)
// 1~MAX_PLAYERS = Valid player IDs
static int int_new_player_id = -1;

void lobby_signal_join(void)
{
	// Signal the main thread to check if
	// number of active players reached
	pthread_mutex_lock(&mutex);
	b_player_join = true;
	pthread_mutex_unlock(&mutex);
}

int lobby_take_new_player_id(void)
{
	// If a new player ID has been assigned by the main thread
	pthread_mutex_lock(&mutex);
	int int_new_player_id_temp = int_new_player_id;
	pthread_mutex_unlock(&mutex);
	if (int_new_player_id_temp != -1)
	{
		pthread_mutex_lock(&mutex);
		// Reset the player ID
		int_new_player_id = -1;
		pthread_mutex_unlock(&mutex);
	}
	return int_new_player_id_temp;
}

void lobby_poll(void)
{
	// If a join request has been received
	pthread_mutex_lock(&mutex);
	bool b_player_join_temp = b_player_join;
	pthread_mutex_unlock(&mutex);
	if (b_player_join_temp == true)
	{
		// Reset the flag
		pthread_mutex_lock(&mutex);
		b_player_join = false;
		pthread_mutex_unlock(&mutex);

		// If max active players reached
		if (int_num_active_players == MAX_PLAYERS)
		{
			// Set invalid player ID
			pthread_mutex_lock(&mutex);
			int_new_player_id = 0;
			pthread_mutex_unlock(&mutex);
		}
		else
		{
			pthread_mutex_lock(&mutex);
			// Loop through the players list
			for(int i = 0; i < MAX_PLAYERS; i++)
			{
				if(arr_Players[i].b_active == false)
				{
					// Set this index to active
					arr_Players[i].b_active = true;
					// Increment the number of active players
					int_num_active_players++;

					// Calculate initial coords of the new player
					int int_rand_x = 0;
					int int_rand_y = 0;
					while(1)
					{
						int_rand_x = rand()%(WIDTH-SQ_WIDTH+1);
						int_rand_y = rand()%(HEIGHT-SQ_WIDTH+1);
						// Function don't have to lock/unlock mutex
						bool b_res = collision_detect(arr_Players,
							MAX_PLAYERS, i+1, int_rand_x, int_rand_y);
						// If no collision detected
						if(b_res == false)
						{
							break;
						}
					}

					// Set the coords
					arr_Players[i].x = int_rand_x;
					arr_Players[i].y = int_rand_y;

					// Set player ID to be the index plus 1
					int_new_player_id = i+1;
					break;
				}
			}
			pthread_mutex_unlock(&mutex);
		}
	}
}
