/* server/net_server.c
Server DDS worker thread: takes inputs, publishes snapshots.

Since #12 the worker publishes one GameSnapshot per published tick (built by
game_tick() on the main thread) instead of one Position message per player,
and an Input carries the held keys, which game_tick() applies. The worker
still busy-polls and still writes while holding the mutex, as the baseline
did: those are optimisations #2 and #1, measured separately.

#14: QoS per topic (common/net.h): join messages reliable and keep-all, input
and snapshots best-effort keep-last 1, and up to INPUT_TAKE inputs taken per
dds_take. FUSE_QOS=default keeps Cyclone's defaults and one input per take.

Join nonce (lobby.h, net_join_nonce() in common/net.h): every JoinRequest
taken is queued for the main thread with its nonce, and every answer is sent
back with that nonce, so only the client that asked accepts it.

Leave (lobby.h, net_leave() in common/net.h): every Leave is authenticated
before queueing removal. Inputs and Leave are checked against the slot's
session under the same mutex as the state update. */

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
	dds_entity_t leave_reader;

	//Pointer to unknown type
	void *samples_join[JOIN_TAKE];
	dds_sample_info_t infos_join[JOIN_TAKE];
	void *samples_input[INPUT_TAKE];
	dds_sample_info_t infos_input[INPUT_TAKE];
	void *samples_leave[JOIN_TAKE];
	dds_sample_info_t infos_leave[JOIN_TAKE];

	dds_return_t rc;

	// Participant and topics
	net_create_topics(&net, NET_ROLE_SERVER);

	/* ----------------------------------------------------- */
	/* Create readers                                        */
	/* ----------------------------------------------------- */
	join_reader = net_create_reader(&net, net.join_topic, NET_RELIABLE_ALL);
	input_reader = net_create_reader(&net, net.input_topic, NET_BEST_EFFORT);
	// A lost Leave would leave the player in the game until the timeout
	leave_reader = net_create_reader(&net, net.leave_topic, NET_RELIABLE_ALL);

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

	JoinResult result;

	// Copy of the latest snapshot, taken from game.c
	GameSnapshot snapshot;

	/* Poll until data has been read. */
	while (1)
	{
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
			if (!lobby_signal_join(p_request))
				LOG_WARN("Join queue full, request dropped\n");
		}

		// Send every answer the main thread has made
		bool b_sent = false;
		while (lobby_take_result(&result))
		{
			rc = dds_write(response_writer, &result);
			if (rc != DDS_RETCODE_OK)
			{
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			}
			LOG_INFO("Join response sent: player %d, status %u\n", (int)result.player_id, result.status);
			b_sent = true;
		}
		/* Polling sleep (the baseline's, once per pass that sent responses). */
		if (b_sent)
			dds_sleepfor (DDS_MSECS (20));

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
			if (!lobby_receive_input(input))
				LOG_DEBUG("Input rejected\n");
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
			if (lobby_receive_leave(p_leave))
				LOG_INFO("Leave accepted from player %d\n", (int)p_leave->player_id);
			else
				LOG_DEBUG("Leave rejected\n");
		}
	}
}
