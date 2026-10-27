/* client/net_client.c
Client DDS worker thread: sends input, receives snapshots. */

#include <stdio.h>

#include "net_client.h"
#include "net.h"
#include "messages.h"
#include "input.h"

int int_player_id = -1;

// Init all members to 0
Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *net_client_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t response_reader;
	dds_entity_t position_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;

	//Pointer to unknown type
	void *samples_response[MAX_SAMPLES];
	dds_sample_info_t infos_response[MAX_SAMPLES];
	void *samples_position[MAX_SAMPLES];
	dds_sample_info_t infos_position[MAX_SAMPLES];
	dds_return_t rc;

	// Participant and topics
	net_create_topics(&net);

	/* ----------------------------------------------------- */
	/* Create writers                                        */
	/* ----------------------------------------------------- */
	join_writer = dds_create_writer(net.participant, net.join_topic, NULL, NULL);
	input_writer = dds_create_writer(net.participant, net.input_topic, NULL, NULL);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	response_reader = dds_create_reader(net.participant, net.response_topic, NULL, NULL);
	position_reader = dds_create_reader(net.participant, net.position_topic, NULL, NULL);

	JoinRequest request;
	request.b_dummy = true;

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_response[0] = JoinResponse__alloc();

	samples_position[0] = Position__alloc();

	// Record the time now
	uint64_t last_join_req_time = dds_time();
	while (1)
	{
		// If not in a game yet
		pthread_mutex_lock(&mutex);
		int int_player_id_temp = int_player_id;
		pthread_mutex_unlock(&mutex);
		if (int_player_id_temp == -1)
		{
			//Listen for server join response
			rc = dds_take(response_reader, samples_response, infos_response, MAX_SAMPLES, MAX_SAMPLES);

			if (rc < 0)
				DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

			/* Check if we read some data and it is valid. */
			if ((rc > 0) && (infos_response[0].valid_data))
			{
				printf("Join response received\n");
				JoinResponse *response = (JoinResponse *) samples_response[0];
				int int_resp_player_id = response->int_player_id;
				// If server responded with an invalid player ID
				if (int_resp_player_id == 0)
					printf("Unable to join\n");
				else
				{
					pthread_mutex_lock(&mutex);
					// Update player ID
					int_player_id = int_resp_player_id;
					pthread_mutex_unlock(&mutex);
				}
			}
			else
			{
				/* Polling sleep. */
				dds_sleepfor (DDS_MSECS (20));
			}

			// Send request every 1s
			// Get the current time
			uint64_t curr_time = dds_time();
			// Get the time difference
			if (curr_time - last_join_req_time > DDS_SECS(1))
			{
				// Update the last req time
				last_join_req_time = curr_time;

				// Send join request
				rc = dds_write(join_writer, &request);
				printf("Join request sent\n");
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
			}
		}

		// Client is an active player
		else
		{
			pthread_mutex_lock(&mutex);
			char ch_dir_temp = ch_dir;
			pthread_mutex_unlock(&mutex);
			// If there is some direction input
			if(ch_dir_temp != 'n')
			{
				// Create a fresh input
				Input input;
				input.int_player_id = int_player_id;

				// Set the direction in the message
				input.ch_direction = ch_dir_temp;
				// Send input
				rc = dds_write(input_writer, &input);
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				printf("Input sent\n");
				// Need to slow down how often input gets sent, otherwise,
				//the player will move to the window limits immediately
				dds_sleepfor(DDS_MSECS(10));
			}
		}

		// Listen for position
		rc = dds_take(position_reader, samples_position, infos_position, MAX_SAMPLES, MAX_SAMPLES);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_position[0].valid_data))
		{
			printf("Position received\n");
			Position *position = (Position *) samples_position[0];
			// Get the player ID
			int int_position_player_id = position->int_player_id;
			pthread_mutex_lock(&mutex);
			// Set this player to active
			arr_Players[int_position_player_id-1].b_active = true;
			// Set this player's x
			arr_Players[int_position_player_id-1].x = position->int_x;
			// Set this player's y
			arr_Players[int_position_player_id-1].y = position->int_y;
			pthread_mutex_unlock(&mutex);
		}
	}
}
