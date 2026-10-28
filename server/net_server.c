/* server/net_server.c
Server DDS worker thread: takes inputs, publishes snapshots.

Since #12 the worker publishes one GameSnapshot per published tick (built by
game_tick() on the main thread) instead of one Position message per player,
and an Input carries the held keys, which game_tick() applies. The worker
still busy-polls and still writes while holding the mutex, as the baseline
did: those are optimisations #2 and #1, measured separately.

#14: QoS per topic (common/net.h): join messages reliable and keep-all, input
and snapshots best-effort keep-last 1, and up to INPUT_TAKE inputs taken per
dds_take. FUSE_QOS=default keeps Cyclone's defaults and one input per take. */

#include <stdio.h>

#include "net_server.h"
#include "net.h"
#include "fuse.h"
#include "game.h"
#include "lobby.h"
#include "log.h"

void *net_server_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t snapshot_writer;
	dds_entity_t input_reader;

	//Pointer to unknown type
	void *samples_join[MAX_SAMPLES];
	dds_sample_info_t infos_join[MAX_SAMPLES];
	void *samples_input[INPUT_TAKE];
	dds_sample_info_t infos_input[INPUT_TAKE];

	dds_return_t rc;

	// Participant and topics
	net_create_topics(&net);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	join_reader = net_create_reader(&net, net.join_topic, NET_RELIABLE_ALL);
	input_reader = net_create_reader(&net, net.input_topic, NET_BEST_EFFORT);

	/* ----------------------------------------------------- */
	/* Create writers                                        */
	/* ----------------------------------------------------- */
	response_writer = net_create_writer(&net, net.response_topic, NET_RELIABLE_ALL);
	snapshot_writer = net_create_writer(&net, net.snapshot_topic, NET_BEST_EFFORT);

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	samples_join[0] = JoinRequest__alloc();
	for (int i = 0; i < INPUT_TAKE; i++)
		samples_input[i] = Input__alloc();
	// Inputs per dds_take: all waiting ones (#14), or one as in the baseline
	int int_input_take = net_qos_tuned() ? INPUT_TAKE : 1;

	// Create a Join Response (nonce and session key stay 0 until the join
	// nonce and login are added)
	JoinResponse response = {0};

	// Copy of the latest snapshot, taken from game.c
	GameSnapshot snapshot;

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
			LOG_INFO("Join request received\n");
			lobby_signal_join();
		}

		// If a new player ID has been assigned by the main thread
		int int_new_player_id_temp = lobby_take_new_player_id();
		if (int_new_player_id_temp != -1)
		{
			// The player ID can be 0, ie invalid (game full)
			response.player_id = (uint8_t)int_new_player_id_temp;
			response.status = int_new_player_id_temp == 0 ? JOIN_FULL : JOIN_OK;

			rc = dds_write(response_writer, &response);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			LOG_INFO("Join response sent\n");
			/* Polling sleep. */
			dds_sleepfor (DDS_MSECS (20));
		}

		// Publish the latest snapshot once (game_tick() on the main thread
		// makes a new one every int_snapshot_every ticks). The write happens
		// while holding the mutex, as the baseline's Position writes did (#1).
		pthread_mutex_lock(&mutex);
		if (game_take_snapshot(&snapshot))
		{
			rc = dds_write(snapshot_writer, &snapshot);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			LOG_DEBUG("Snapshot %u sent\n", (unsigned)snapshot.tick);
		}
		pthread_mutex_unlock(&mutex);

		// Listen for input
		rc = dds_take(input_reader, samples_input, infos_input,
		              (size_t)int_input_take, (uint32_t)int_input_take);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		// Every valid input taken, oldest first (a later one from the same
		// player overwrites the keys of an earlier one)
		for (int i = 0; i < rc; i++)
		{
			if (!infos_input[i].valid_data)
				continue;
			LOG_DEBUG("Input received\n");
			Input *input = (Input *) samples_input[i];
			// Remember the held keys; the next tick moves the player
			game_set_keys(input->player_id, input->keys);
		}
	}
}
