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
of ~100. --input-send every-10ms keeps the old behaviour for measurements.

Join nonce (common/net.h): the client sends the same random nonce in every
JoinRequest and accepts only the JoinResponse carrying it; responses to other
clients are taken and dropped. FUSE_JOIN_NONCE=off: nonce 0 and the first
response is taken, as in the baseline.

Leave (common/net.h): when the window closes, the worker sends a Leave so the
server frees the slot at once. If the server dropped this client (no Input
for its timeout, e.g. the network was cut), the client sees its own slot
missing from the snapshots and joins again with a new nonce. */

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
GameSnapshot latest_snapshot = {0};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Set by net_client_stop() (main thread), read by the worker, under the mutex
static bool b_quit = false;

void net_client_stop(void)
{
	pthread_mutex_lock(&mutex);
	b_quit = true;
	pthread_mutex_unlock(&mutex);
}

void *net_client_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t response_reader;
	dds_entity_t snapshot_reader;
	dds_entity_t join_writer;
	dds_entity_t input_writer;
	dds_entity_t leave_writer;

	//Pointer to unknown type
	void *samples_response[JOIN_TAKE];
	dds_sample_info_t infos_response[JOIN_TAKE];
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
	// Reliable: a lost Leave would keep the player in the game until the timeout
	leave_writer = net_create_writer(&net, net.leave_topic, NET_RELIABLE_ALL);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
	snapshot_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);

	// One nonce for this client, the same in every request (so the server
	// knows a repeated request); name and proof stay empty until the login is
	// added
	JoinRequest request = {0};
	bool b_join_nonce = net_join_nonce();
	if (b_join_nonce)
		request.nonce = net_random_nonce();

	// Input message: sequence number counts every input sent
	Input input = {0};
	// Keys in the last input sent, to send one more input when they are released
	uint8_t u8_sent_keys = 0;
	// When the last input was sent (#13 repeats it every INPUT_REPEAT_MS)
	dds_time_t last_input_time = 0;
	// True once our own player has appeared in a snapshot; if it disappears
	// after that, the server has dropped us (snapshots taken before the
	// server admitted us do not show us yet, so they must not count)
	bool b_seen_self = false;

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	for (int i = 0; i < JOIN_TAKE; i++)
		samples_response[i] = JoinResponse__alloc();

	samples_snapshot[0] = GameSnapshot__alloc();

	// Record the time now
	uint64_t last_join_req_time = dds_time();
	while (1)
	{
		// If not in a game yet
		pthread_mutex_lock(&mutex);
		int int_player_id_temp = int_player_id;
		bool b_quit_temp = b_quit;
		pthread_mutex_unlock(&mutex);
		// The window was closed
		if (b_quit_temp)
			break;
		if (int_player_id_temp == -1)
		{
			//Listen for server join response
			// Every waiting response (every client receives all of them)
			rc = dds_take(response_reader, samples_response, infos_response, JOIN_TAKE, JOIN_TAKE);

			if (rc < 0)
				DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

			int int_resp_player_id = -1;
			for (int i = 0; i < rc && int_resp_player_id == -1; i++)
			{
				JoinResponse *response = (JoinResponse *) samples_response[i];
				// Only the response to our own request (any one without nonce)
				if (!infos_response[i].valid_data ||
				    (b_join_nonce && response->nonce != request.nonce))
					continue;
				LOG_INFO("Join response received\n");
				// If server responded with an invalid player ID
				if (response->status != JOIN_OK || response->player_id == 0)
				{
					LOG_WARN("Unable to join\n");
					int_resp_player_id = 0;
				}
				else
					int_resp_player_id = response->player_id;
			}

			if (int_resp_player_id > 0)
			{
				pthread_mutex_lock(&mutex);
				// Update player ID
				int_player_id = int_resp_player_id;
				pthread_mutex_unlock(&mutex);
			}
			else if (rc == 0)
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
			latest_snapshot = *snapshot;
			for (int i = 0; i < MAX_PLAYERS; i++)
			{
				const PlayerState *p_state = &snapshot->players[i];
				arr_Players[i].b_active = (p_state->flags & PF_ACTIVE) != 0;
				arr_Players[i].x = p_state->x;
				arr_Players[i].y = p_state->y;
			}

			// Our own slot (if we are playing): still there?
			if (int_player_id_temp >= 1 && int_player_id_temp <= MAX_PLAYERS && net_leave())
			{
				if (arr_Players[int_player_id_temp - 1].b_active)
					b_seen_self = true;
				else if (b_seen_self)
				{
					// Dropped by the server: join again. A new nonce, so a
					// late response to the old one cannot be taken by mistake.
					LOG_WARN("Removed by the server (timed out), joining again\n");
					int_player_id = -1;
					b_seen_self = false;
					if (b_join_nonce)
						request.nonce = net_random_nonce();
				}
			}
			pthread_mutex_unlock(&mutex);
		}
	}

	// Tell the server we are leaving, so it frees the slot now instead of
	// after the timeout, and wait (briefly) until it has the message: the
	// process exits right after this
	if (int_player_id > 0 && net_leave())
	{
		Leave leave = {0};
		leave.player_id = (uint8_t)int_player_id;
		leave.seq = ++input.seq;   // continues the Input sequence (see the IDL)
		rc = dds_write(leave_writer, &leave);
		if (rc != DDS_RETCODE_OK)
			LOG_WARN("Leave not sent: %s\n", dds_strretcode(-rc));
		else if (dds_wait_for_acks(leave_writer, DDS_MSECS(500)) != DDS_RETCODE_OK)
			LOG_WARN("Leave not acknowledged (server gone?)\n");
		else
			LOG_INFO("Left the game\n");
	}
	dds_delete(net.participant);
	return NULL;
}
