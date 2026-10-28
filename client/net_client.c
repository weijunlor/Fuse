/* client/net_client.c
Client DDS worker thread: sends input, receives snapshots.

Since #12 the client receives one GameSnapshot per published server tick
(every player at once) instead of one Position message per player, and sends
the keys it holds instead of single steps: every 10 ms while any key is held
(the baseline's rate), plus once when the last key is released so the server
stops the player.

#13 (input on change, the default): an Input is sent only when the held keys
change, and repeated every INPUT_REPEAT_MS in case one was lost (Input will be
best-effort, #14 in common/net.h). A held arrow key then costs ~10 messages per second instead
of ~100. --input-send every-10ms keeps the old behaviour for measurements. */

#include <stdio.h>

#include "net_client.h"
#include "net.h"
#include "fuse.h"
#include "input.h"
#include "log.h"

int int_player_id = -1;

bool b_input_on_change = true;

// #13: repeat the current keys this often even if they did not change, so a
// lost Input is corrected within this time
#define INPUT_REPEAT_MS 100

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
	join_writer = net_create_writer(&net, net.join_topic, NET_RELIABLE_ALL);
	input_writer = net_create_writer(&net, net.input_topic, NET_BEST_EFFORT);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
	snapshot_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);

	// Nonce, name and proof stay empty until the join nonce and login are added
	JoinRequest request = {0};

	// Input message: sequence number counts every input sent
	Input input = {0};
	// Keys in the last input sent, to send one more input when they are released
	uint8_t u8_sent_keys = 0;
	// When the last input was sent (#13 repeats it every INPUT_REPEAT_MS)
	dds_time_t last_input_time = 0;

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

			bool b_send;
			if (b_input_on_change)
			{
				// #13: when the keys changed, or INPUT_REPEAT_MS after the
				// last input (held or not, so a lost release is resent too)
				dds_time_t now = dds_time();
				b_send = u8_keys_temp != u8_sent_keys ||
				         now - last_input_time >= DDS_MSECS(INPUT_REPEAT_MS);
				if (b_send)
					last_input_time = now;
			}
			else
			{
				// Every 10 ms while keys are held, and once more when released
				b_send = u8_keys_temp != 0 || u8_sent_keys != 0;
			}

			if (b_send)
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
			}

			// While a key is held the loop waits 10 ms per pass in both modes,
			// as the baseline did after each input (keys are read every 10 ms
			// in both, so the modes differ only in how many inputs they send).
			// Otherwise it busy-polls, as the baseline did; waiting on DDS and
			// the keyboard instead of polling is #2.
			if (u8_keys_temp != 0 || (b_send && !b_input_on_change))
				dds_sleepfor(DDS_MSECS(10));
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
