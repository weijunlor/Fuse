/* client/net_client.c
Client DDS worker thread: sends input, receives snapshots.

Since #12 the client receives one GameSnapshot per published server tick
(every player at once) instead of one Position message per player, and sends
the keys it holds instead of single steps: every 10 ms while any key is held
(the baseline's rate), plus once when the last key is released so the server
stops the player. Sending only on change is #13. */

#include <stdio.h>

#include "net_client.h"
#include "net.h"
#include "fuse.h"
#include "input.h"
#include "log.h"

int int_player_id = -1;

// Init all members to 0
Player arr_Players[MAX_PLAYERS] = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *net_client_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t response_reader;
	dds_entity_t snapshot_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;

	//Pointer to unknown type
	void *samples_response[MAX_SAMPLES];
	dds_sample_info_t infos_response[MAX_SAMPLES];
	void *samples_snapshot[MAX_SAMPLES];
	dds_sample_info_t infos_snapshot[MAX_SAMPLES];
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
	snapshot_reader = dds_create_reader(net.participant, net.snapshot_topic, NULL, NULL);

	// Nonce, name and proof stay empty until the join nonce and login are added
	JoinRequest request = {0};

	// Input message: sequence number counts every input sent
	Input input = {0};
	// Keys in the last input sent, to send one more input when they are released
	uint8_t u8_sent_keys = 0;

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_response[0] = JoinResponse__alloc();

	samples_snapshot[0] = GameSnapshot__alloc();

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
				LOG_INFO("Join response received\n");
				JoinResponse *response = (JoinResponse *) samples_response[0];
				int int_resp_player_id = response->player_id;
				// If server responded with an invalid player ID
				if (response->status != JOIN_OK || int_resp_player_id == 0)
					LOG_WARN("Unable to join\n");
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
				LOG_INFO("Join request sent\n");
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
			uint8_t u8_keys_temp = u8_keys;
			pthread_mutex_unlock(&mutex);
			// While keys are held, and once more when they are released
			if (u8_keys_temp != 0 || u8_sent_keys != 0)
			{
				input.player_id = (uint8_t)int_player_id;
				input.seq++;
				input.keys = u8_keys_temp;
				// Send input
				rc = dds_write(input_writer, &input);
				if (rc != DDS_RETCODE_OK)
				{
					DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
				}
				LOG_DEBUG("Input sent: keys 0x%02x\n", (unsigned)u8_keys_temp);
				u8_sent_keys = u8_keys_temp;
				// Same rate as the baseline: one input per 10 ms while held
				dds_sleepfor(DDS_MSECS(10));
			}
		}

		// Listen for snapshots
		rc = dds_take(snapshot_reader, samples_snapshot, infos_snapshot, MAX_SAMPLES, MAX_SAMPLES);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Check if we read some data and it is valid.
		if ((rc > 0) && (infos_snapshot[0].valid_data))
		{
			GameSnapshot *snapshot = (GameSnapshot *) samples_snapshot[0];
			LOG_DEBUG("Snapshot %u received\n", (unsigned)snapshot->tick);
			// Copy every player at once, so a frame never shows a mix of
			// two ticks (the baseline updated one player per message)
			pthread_mutex_lock(&mutex);
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				const PlayerState *p_state = &snapshot->players[i];
				arr_Players[i].b_active = (p_state->flags & PF_ACTIVE) != 0;
				arr_Players[i].x = p_state->x;
				arr_Players[i].y = p_state->y;
			}
			pthread_mutex_unlock(&mutex);
		}
	}
}
