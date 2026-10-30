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
clients are taken and dropped. Authenticated joins always use nonces and a
password proof. FUSE_JOIN_NONCE=off only applies in unauthenticated comparison
mode: nonce 0 and the first response is taken, as in the baseline.

Leave (common/net.h): when the window closes, the worker sends a Leave so the
server frees the slot at once. If the server dropped this client (no Input
for its timeout, e.g. the network was cut), the client sees its own slot
missing from the snapshots and joins again with a new nonce. */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

#include "net_client.h"
#include "net.h"
#include "fuse.h"
#include "log.h"
#include "app_client.h"
#include "handoff.h"
#include "double_buffer.h"

DOUBLE_BUFFER_DEFINE(ClientFrameBuffer, ClientFrame)
static ClientFrameBuffer frame_buffer;
static ClientFrame frame_mutex = { .int_player_id = -1 };
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_uchar u8_keys;

bool b_input_on_change = true;

// #13: repeat the current keys this often even if they did not change, so a
// lost Input is corrected within this time
#define INPUT_REPEAT_MS 100

// Independent scalar handoffs need no player-table mutex.
static atomic_bool b_quit;
static AppClient client_auth;

bool net_client_prepare(void)
{
	if (!handoff_init())
		return false;
	ClientFrameBuffer_init(&frame_buffer);
	frame_mutex = (ClientFrame){ .int_player_id = -1 };
	atomic_init(&u8_keys, 0);
	atomic_init(&b_quit, false);
	return app_client_init(&client_auth);
}

void net_client_set_keys(uint8_t a_u8_keys)
{
	atomic_store_explicit(&u8_keys, a_u8_keys, memory_order_relaxed);
}

bool net_client_read_frame(ClientFrame *a_frame)
{
	if (handoff_spsc())
		return ClientFrameBuffer_take(&frame_buffer, a_frame);
	pthread_mutex_lock(&mutex);
	*a_frame = frame_mutex;
	pthread_mutex_unlock(&mutex);
	return true;
}

// Called only by the worker. A slow reader may hold the other slot; skip
// that publication and let the next snapshot replace it, without waiting.
static void publish_frame(const ClientFrame *a_frame)
{
	if (handoff_spsc())
		ClientFrameBuffer_publish(&frame_buffer, a_frame);
	else
	{
		pthread_mutex_lock(&mutex);
		frame_mutex = *a_frame;
		pthread_mutex_unlock(&mutex);
	}
}

void net_client_stop(void)
{
	atomic_store_explicit(&b_quit, true, memory_order_relaxed);
}

void *net_client_worker(void *arg)
{
	(void)arg;
	// All mutable session/ID/snapshot state belongs to this worker. The
	// main thread gets copied frames, never references into this object.
	ClientFrame frame = { .int_player_id = -1 };

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
	net_create_topics(&net, NET_ROLE_CLIENT);

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

	// A retry reuses this nonce/proof; a new session always gets a new nonce.
	JoinRequest request = {0};
	bool b_join_nonce = client_auth.b_enabled || net_join_nonce();
	if (!app_client_request(&client_auth, &request))
		DDS_FATAL("Cannot create login proof\n");

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
		int int_player_id_temp = frame.int_player_id;
		bool b_quit_temp = atomic_load_explicit(&b_quit, memory_order_relaxed);
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
				if (response->status == JOIN_BAD_LOGIN && client_auth.b_enabled)
					DDS_FATAL("Login rejected (credentials, active account, or replay check)\n");
				if (!app_client_response(&client_auth, &request, response))
				{
					LOG_WARN("Unable to join\n");
					int_resp_player_id = 0;
				}
				else
					int_resp_player_id = response->player_id;
			}

			if (int_resp_player_id > 0)
			{
				frame.int_player_id = int_resp_player_id;
				publish_frame(&frame);
				u8_sent_keys = 0;
				last_input_time = 0;
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
			uint8_t u8_keys_temp = atomic_load_explicit(&u8_keys, memory_order_relaxed);

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
				input.player_id = (uint8_t)int_player_id_temp;
				input.keys = u8_keys_temp;
				if (!app_client_input(&client_auth, &input))
					DDS_FATAL("Input signing failed or sequence exhausted; reconnect\n");
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
			frame.snapshot = *snapshot;

			// Our own slot (if we are playing): still there?
			if (int_player_id_temp >= 1 && int_player_id_temp <= MAX_PLAYERS && net_leave())
			{
				if (snapshot->players[int_player_id_temp - 1].flags & PF_ACTIVE)
					b_seen_self = true;
				else if (b_seen_self)
				{
					// Dropped by the server: join again. A new nonce, so a
					// late response to the old one cannot be taken by mistake.
					LOG_WARN("Removed by the server (timed out), joining again\n");
					frame.int_player_id = -1;
					b_seen_self = false;
					crypto_clear(client_auth.arr_session_key, sizeof client_auth.arr_session_key);
					if (!app_client_request(&client_auth, &request))
						DDS_FATAL("Cannot create rejoin proof\n");
				}
			}
			publish_frame(&frame);
		}
	}

	// Tell the server we are leaving, so it frees the slot now instead of
	// after the timeout, and wait (briefly) until it has the message: the
	// process exits right after this
	if (frame.int_player_id > 0 && net_leave())
	{
		Leave leave = {0};
		leave.player_id = (uint8_t)frame.int_player_id;
		if (!app_client_leave(&client_auth, &leave))
			DDS_FATAL("Leave signing failed or sequence exhausted\n");
		rc = dds_write(leave_writer, &leave);
		if (rc != DDS_RETCODE_OK)
			LOG_WARN("Leave not sent: %s\n", dds_strretcode(-rc));
		else if (dds_wait_for_acks(leave_writer, DDS_MSECS(500)) != DDS_RETCODE_OK)
			LOG_WARN("Leave not acknowledged (server gone?)\n");
		else
			LOG_INFO("Left the game\n");
	}
	dds_delete(net.participant);
	crypto_clear(&client_auth, sizeof client_auth);
	return NULL;
}
