/* server/net_server.c
Server DDS worker thread: takes inputs, publishes snapshots.

Since #12 the worker publishes one GameSnapshot per published tick (built by
game_tick() on the main thread) instead of one Position message per player,
and an Input carries the held keys, which game_tick() applies. The worker
blocks on a waitset (#2) and publishes a copied snapshot outside the mutex
(#1). --wait busy and --publish locked retain the separate before paths.
and an Input carries the held keys, which game_tick() applies. Waiting
instead of busy-polling (#2) and writing outside the mutex (#1) are described
at the end of this comment; both keep the baseline's way behind a switch.

#14: QoS per topic (common/net.h): join messages reliable and keep-all, input
and snapshots best-effort keep-last 1, and up to INPUT_TAKE inputs taken per
dds_take. FUSE_QOS=default keeps Cyclone's defaults and one input per take.

Join nonce (lobby.h, net_join_nonce() in common/net.h): every JoinRequest
taken is queued for the main thread with its nonce, and every answer is sent
back with that nonce, so only the client that asked accepts it.

Leave (lobby.h, net_leave() in common/net.h): every Leave taken is queued for
the main thread, which frees the player's slot.

#2 (wait instead of spinning): the worker blocks in a waitset on three read
conditions (join, input, leave) and a guard condition that the main thread
sets after each tick (net_server_wake()), so it wakes only when there is work:
a message to take, or a snapshot or join answer to send. The baseline polled
the readers in a busy loop. FUSE_SERVER_WAIT=spin keeps the busy loop.

#1 (no network write under the mutex): the snapshot is copied under the mutex
and written after unlocking, so the main thread's tick never waits for
dds_write(). FUSE_WRITE_UNDER_LOCK=on keeps the write inside the lock. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_server.h"
#include "net.h"
#include "fuse.h"
#include "game.h"
#include "lobby.h"
#include "log.h"

bool b_server_wait = true;
bool b_publish_locked = false;
bool b_write_under_lock = false;

// Set by net_server_wake() (main thread), waited on by the worker. A guard
// condition is thread-safe, so no mutex is needed around it.
static dds_entity_t guard_wake = 0;

void net_server_init(void)
{
	const char *str_wait = getenv("FUSE_SERVER_WAIT");
	b_server_wait = !(str_wait != NULL && strcmp(str_wait, "spin") == 0);
	const char *str_lock = getenv("FUSE_WRITE_UNDER_LOCK");
	b_write_under_lock = str_lock != NULL && strcmp(str_lock, "on") == 0;
	LOG_INFO("Server wait: %s, snapshot write: %s\n",
	         b_server_wait ? "sleep and waitset" : "spin (as the baseline)",
	         b_write_under_lock ? "under the mutex (as the baseline)" : "after unlocking");

	if (b_server_wait)
	{
		// Owned by the library, not the participant (the worker creates that
		// later), so the waitset is created on the library handle too
		guard_wake = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE);
		if (guard_wake < 0)
			DDS_FATAL("dds_create_guardcondition: %s\n", dds_strretcode(-guard_wake));
	}
}

void net_server_wake(void)
{
	if (guard_wake > 0)
		dds_set_guardcondition(guard_wake, true);
}

void *net_server_worker(void *arg)
{
	(void)arg;

	NetTopics net;
	dds_entity_t join_reader;
	dds_entity_t response_writer;
	dds_entity_t snapshot_writer;
	dds_entity_t input_reader;
	dds_entity_t leave_reader;
	dds_entity_t waitset = 0;

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

	bool b_leave = net_leave();

	/* #2: block until a reader has a sample or the main thread sets the
	guard condition. A read condition with DDS_ANY_STATE stays triggered while
	any sample is in the reader, so every reader attached here must be emptied
	each pass: with FUSE_LEAVE=off Leaves are never taken, so the leave reader
	is not attached (or the waitset would never block again). */
	if (b_server_wait)
	{
		waitset = dds_create_waitset(DDS_CYCLONEDDS_HANDLE);
		if (waitset < 0)
			DDS_FATAL("dds_create_waitset: %s\n", dds_strretcode(-waitset));
		dds_entity_t arr_readers[3] = { join_reader, input_reader, leave_reader };
		int int_num_readers = b_leave ? 3 : 2;
		for (int i = 0; i < int_num_readers; i++)
		{
			dds_entity_t condition = dds_create_readcondition(arr_readers[i], DDS_ANY_STATE);
			if (condition < 0)
				DDS_FATAL("dds_create_readcondition: %s\n", dds_strretcode(-condition));
			rc = dds_waitset_attach(waitset, condition, arr_readers[i]);
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_waitset_attach: %s\n", dds_strretcode(-rc));
		}
		rc = dds_waitset_attach(waitset, guard_wake, 0);
		if (rc != DDS_RETCODE_OK)
			DDS_FATAL("dds_waitset_attach: %s\n", dds_strretcode(-rc));
	}

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
	// Inputs per dds_take: all waiting ones (#14), or one as in the baseline
	int int_input_take = net_qos_tuned() ? INPUT_TAKE : 1;

	// Join Response (the session key stays 0 until the login is added)
	JoinResponse response = {0};
	JoinResult result;

	// Copy of the latest snapshot, taken from game.c
	GameSnapshot snapshot;
	dds_attach_t arr_triggered[4];

	while (1)
	{
		if (b_server_wait)
		{
			rc = dds_waitset_wait(waitset, triggered, 3, DDS_MSECS(10));
			if (rc < 0)
				DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-rc));
			// Sleep until there is work. No timeout: the main thread wakes us
			// after every tick, and the readers wake us when data arrives.
			rc = dds_waitset_wait(waitset, arr_triggered, 4, DDS_INFINITY);
			if (rc < 0)
				DDS_FATAL("dds_waitset_wait: %s\n", dds_strretcode(-rc));
			// Reset the guard before the work below, so a wake-up that comes
			// while we work is not lost (it makes the next wait return at once)
			bool b_woken;
			dds_take_guardcondition(guard_wake, &b_woken);
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
		/* Polling sleep (the baseline's, once per pass that sent responses).
		Only in spin mode: it holds up every snapshot for 20 ms. */
		if (b_sent && !b_server_wait)
			dds_sleepfor (DDS_MSECS (20));

		// Publish the latest snapshot (game_tick() makes a new one every
		// int_snapshot_every ticks). #1: copy it under the mutex and write it
		// after unlocking; FUSE_WRITE_UNDER_LOCK=on writes while locked.
		pthread_mutex_lock(&mutex);
		b_has_snapshot = game_take_snapshot(&snapshot);
		if (!b_publish_locked)
			pthread_mutex_unlock(&mutex);
		bool b_has_snapshot = game_take_snapshot(&snapshot);
		if (b_has_snapshot && b_write_under_lock)
		{
			rc = dds_write(snapshot_writer, &snapshot);
			b_has_snapshot = false;   // written
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
			LOG_DEBUG("Snapshot %u sent\n", (unsigned)snapshot.tick);
		}
		pthread_mutex_unlock(&mutex);

		if (b_has_snapshot)
		{
			rc = dds_write(snapshot_writer, &snapshot);
			if (rc != DDS_RETCODE_OK)
				DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
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
