/* server/net_server.c
Server DDS worker thread: takes inputs, publishes snapshots. */

#include <stdio.h>

#include "net_server.h"
#include "net.h"
#include "messages.h"
#include "game.h"
#include "lobby.h"

void *net_server_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t position_writer;
	dds_entity_t input_reader;

	//Pointer to unknown type
	void *samples_join[MAX_SAMPLES];
	dds_sample_info_t infos_join[MAX_SAMPLES];
	void *samples_input[MAX_SAMPLES];
	dds_sample_info_t infos_input[MAX_SAMPLES];

	dds_return_t rc;

	// Participant and topics
	net_create_topics(&net);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	join_reader = dds_create_reader(net.participant, net.join_topic, NULL, NULL);
	input_reader = dds_create_reader(net.participant, net.input_topic, NULL, NULL);

	/* ----------------------------------------------------- */
	/* Create writers                                        */
	/* ----------------------------------------------------- */
	response_writer = dds_create_writer(net.participant, net.response_topic, NULL, NULL);
	position_writer = dds_create_writer(net.participant, net.position_topic, NULL, NULL);

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	samples_input[0] = Input__alloc();

	// Create a Join Response
	JoinResponse response;

	// Create a Position
	Position position;

	// Record the time now
	uint64_t last_time = dds_time();
	/* Poll until data has been read. */
	while (1)
	{
		// Process a join request
		rc = dds_take(join_reader, samples_join, infos_join, MAX_SAMPLES, MAX_SAMPLES);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check for valid join request
		if ((rc > 0) && (infos_join[0].valid_data))
		{
			printf("Join request received\n");
			lobby_signal_join();
		}

		// If a new player ID has been assigned by the main thread
		int int_new_player_id_temp = lobby_take_new_player_id();
		if (int_new_player_id_temp != -1)
		{
			// The player ID can be 0, ie invalid
			response.int_player_id = int_new_player_id_temp;

			rc = dds_write(response_writer, &response);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			printf("Join response sent\n");
			/* Polling sleep. */
			dds_sleepfor (DDS_MSECS (20));
		}

		// Get the current time
		uint64_t curr_time = dds_time();
		// Get the time difference
		if (curr_time - last_time > DDS_SECS(0.1))
		{
			// Update the last time
			last_time = curr_time;

			// Loop through the players list
			for(int i = 0; i < MAX_PLAYERS; i++)
			{
				pthread_mutex_lock(&mutex);
				// If player is active
				if(arr_Players[i].b_active == true)
				{
					position.int_player_id = i+1;
					position.int_x = arr_Players[i].x;
					position.int_y = arr_Players[i].y;
					rc = dds_write(position_writer, &position);
					if (rc != DDS_RETCODE_OK)
					{
						DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
					}
					printf("Position sent\n");
					/* Polling sleep. */
					dds_sleepfor (DDS_MSECS (20));
				}
				pthread_mutex_unlock(&mutex);
			}
		}

		// Listen for input
		rc = dds_take(input_reader, samples_input, infos_input, MAX_SAMPLES, MAX_SAMPLES);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_input[0].valid_data))
		{
			printf("Input received\n");
			Input *input = (Input *) samples_input[0];
			// Move the player in the input direction
			game_apply_input(input->int_player_id, input->ch_direction);
		}
	}
}
