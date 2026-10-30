/* server/net_server.c
Server DDS worker thread: takes inputs, publishes snapshots.

Since #12 the worker publishes one GameSnapshot per published tick (built by
game_tick() on the main thread) instead of one Position message per player,
and an Input carries the held keys, which game_tick() applies. The worker
blocks on a waitset (#2) and publishes a copied snapshot outside the mutex
(#1). --wait busy and --publish locked retain the separate before paths.

#14: QoS per topic (common/net.h): join messages reliable and keep-all, input
and snapshots best-effort keep-last 1, and up to INPUT_TAKE inputs taken per
dds_take. FUSE_QOS=default keeps Cyclone's defaults and one input per take.

Join nonce (lobby.h, net_join_nonce() in common/net.h): every JoinRequest
taken is queued for the main thread with its nonce, and every answer is sent
back with that nonce, so only the client that asked accepts it.

Leave (lobby.h, net_leave() in common/net.h): every Leave taken is queued for
the main thread, which frees the player's slot. */

#include <stdio.h>

#include "net_server.h"
#include "net.h"
#include "fuse.h"
#include "game.h"
#include "lobby.h"
#include "log.h"

bool b_server_wait = true;
bool b_publish_locked = false;

void *net_server_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t snapshot_writer;
	dds_entity_t input_reader;
	dds_entity_t leave_reader;
	dds_entity_t waitset;
	dds_entity_t join_condition;
	dds_entity_t input_condition;
	dds_entity_t leave_condition;

	//Pointer to unknown type
	void *samples_join[JOIN_TAKE];
	dds_sample_info_t infos_join[JOIN_TAKE];
	void *samples_input[INPUT_TAKE];
	dds_sample_info_t infos_input[INPUT_TAKE];
	void *samples_leave[JOIN_TAKE];
	dds_sample_info_t infos_leave[JOIN_TAKE];

	dds_return_t rc;

	// Participant and topics
	net_create_topics(&net);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	join_reader = net_create_reader(&net, net.join_topic, NET_RELIABLE_ALL);
	input_reader = net_create_reader(&net, net.input_topic, NET_BEST_EFFORT);
	// A lost Leave would leave the player in the game until the timeout
	leave_reader = net_create_reader(&net, net.leave_topic, NET_RELIABLE_ALL);

	/* Wait for network data instead of polling the readers continuously.
	 * The timeout lets the worker publish snapshots made by game_tick(). */
	waitset = dds_create_waitset(net.participant);
	if (waitset < 0)
		DDS_FATAL("dds_create_waitset: %s\n", dds_strretcode(-waitset));
	join_condition = dds_create_readcondition(join_reader, DDS_ANY_STATE);
	input_condition = dds_create_readcondition(input_reader, DDS_ANY_STATE);
	leave_condition = dds_create_readcondition(leave_reader, DDS_ANY_STATE);
	if (join_condition < 0 || input_condition < 0 || leave_condition < 0)
		DDS_FATAL("dds_create_readcondition failed\n");
	if (dds_waitset_attach(waitset, join_condition, join_reader) != DDS_RETCODE_OK ||
	    dds_waitset_attach(waitset, input_condition, input_reader) != DDS_RETCODE_OK ||
	    dds_waitset_attach(waitset, leave_condition, leave_reader) != DDS_RETCODE_OK)
		DDS_FATAL("dds_waitset_attach failed\n");

	/* ----------------------------------------------------- */
	/* Create writers                                        */
	/* ----------------------------------------------------- */
	response_writer = net_create_writer(&net, net.response_topic, NET_RELIABLE_ALL);
	snapshot_writer = net_create_writer(&net, net.snapshot_topic, NET_BEST_EFFORT);

	/* Initialize sample buffer, by pointing the void pointer within
	* the buffer array to a valid sample memory location. */
	for (int i = 0; i < JOIN_TAKE; i++)
		samples_join[i] = JoinRequest__alloc();
	for (int i = 0; i < INPUT_TAKE; i++)
		samples_input[i] = Input__alloc();
	for (int i = 0; i < JOIN_TAKE; i++)
		samples_leave[i] = Leave__alloc();
	bool b_leave = net_leave();
	// Inputs per dds_take: all waiting ones (#14), or one as in the baseline
	int int_input_take = net_qos_tuned() ? INPUT_TAKE : 1;

	// Join Response (the session key stays 0 until the login is added)
	JoinResponse response = {0};
	JoinResult result;

	// Copy of the latest snapshot, taken from game.c
	GameSnapshot snapshot;
	dds_attach_t triggered[3];

	/* Wait for data or periodically wake to publish a snapshot. */
	while (1)
	{
		if (b_server_wait)
		{
			rc = dds_waitset_wait(waitset, triggered, 3, DDS_MSECS(10));
			if (rc < 0)
				DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-rc));
		}

		// Queue every waiting join request for the main thread (the baseline
		// took one per pass and merged requests that arrived together)
		rc = dds_take(join_reader, samples_join, infos_join, JOIN_TAKE, JOIN_TAKE);

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		for (int i = 0; i < rc; i++)
		{
			if (!infos_join[i].valid_data)
				continue;
			const JoinRequest *p_request = samples_join[i];
			LOG_INFO("Join request received (nonce %016llx)\n",
			         (unsigned long long)p_request->nonce);
			if (!lobby_signal_join(p_request->nonce))
				LOG_WARN("Join queue full, request dropped\n");
		}

		// Send every answer the main thread has made
		while (lobby_take_result(&result))
		{
			response.nonce = result.u64_nonce;
			response.status = result.u8_status;
			response.player_id = result.u8_player_id;

			rc = dds_write(response_writer, &response);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			LOG_INFO("Join response sent: status %u, player %u\n",
			         result.u8_status, result.u8_player_id);
		}

		// Copy the latest snapshot while locked, then publish it without
		// holding the game mutex. game_tick() makes a new snapshot every
		// int_snapshot_every ticks.
		bool b_has_snapshot;
		pthread_mutex_lock(&mutex);
		b_has_snapshot = game_take_snapshot(&snapshot);
		if (!b_publish_locked)
			pthread_mutex_unlock(&mutex);

		if (b_has_snapshot)
		{
			rc = dds_write(snapshot_writer, &snapshot);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			LOG_DEBUG("Snapshot %u sent\n", (unsigned)snapshot.tick);
		}
		if (b_publish_locked)
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

		// Players who quit: queue them for the main thread to remove
		// (FUSE_LEAVE=off: not even looked at, so the loop is the old one)
		rc = b_leave ? dds_take(leave_reader, samples_leave, infos_leave, JOIN_TAKE, JOIN_TAKE) : 0;

		if (rc < 0)
			DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));

		for (int i = 0; i < rc; i++)
		{
			if (!infos_leave[i].valid_data)
				continue;
			const Leave *p_leave = samples_leave[i];
			LOG_INFO("Leave received from player %d\n", (int)p_leave->player_id);
			if (!lobby_signal_leave(p_leave->player_id))
				LOG_WARN("Leave queue full, Leave dropped\n");
		}
	}
}
