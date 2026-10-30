/* Real DDS integration fixture. The runner supplies an isolated discovery tag. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "net.h"
#include "types.h"
#include "config.h"
#include "fuse.h"

static dds_entity_t response_reader, snapshot_reader, join_writer, input_writer;
static uint8_t arr_ids[2], arr_keys[2];
static uint32_t arr_seq[2];
static dds_time_t last_send;
static GameSnapshot latest;

static void pump(void)
{
	if (dds_time() - last_send >= DDS_MSECS(50))
	{
		for (int i = 0; i < 2; i++)
		{
			if (!arr_ids[i]) continue;
			Input input = {.player_id = arr_ids[i], .seq = ++arr_seq[i], .keys = arr_keys[i]};
			assert(dds_write(input_writer, &input) == DDS_RETCODE_OK);
		}
		last_send = dds_time();
	}
	GameSnapshot sample;
	void *samples[] = {&sample};
	dds_sample_info_t info;
	int count = dds_take(snapshot_reader, samples, &info, 1, 1);
	assert(count >= 0);
	if (count == 1 && info.valid_data) latest = sample;
	dds_sleepfor(DDS_MSECS(2));
}

static JoinResponse join(uint64_t a_nonce)
{
	JoinRequest request = {.nonce = a_nonce};
	dds_time_t deadline = dds_time() + DDS_SECS(4), last_request = 0;
	while (dds_time() < deadline)
	{
		if (dds_time() - last_request >= DDS_MSECS(200))
		{
			assert(dds_write(join_writer, &request) == DDS_RETCODE_OK);
			last_request = dds_time();
		}
		JoinResponse response;
		void *samples[] = {&response};
		dds_sample_info_t info;
		int count = dds_take(response_reader, samples, &info, 1, 1);
		assert(count >= 0);
		if (count == 1 && info.valid_data && response.nonce == a_nonce) return response;
		pump();
	}
	assert(!"join timed out");
	return (JoinResponse){0};
}

static void wait_phase(uint8_t a_phase, int a_seconds)
{
	dds_time_t deadline = dds_time() + DDS_SECS(a_seconds);
	while (dds_time() < deadline)
	{
		pump();
		if (latest.tick && latest.phase == a_phase) return;
	}
	fprintf(stderr, "Expected phase %u, got %u at tick %u\n", a_phase, latest.phase, latest.tick);
	assert(!"phase timed out");
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	int bots = atoi(argv[1]);
	assert(bots == 0 || bots == 1);
	NetTopics net;
	net_create_topics(&net);
	response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
	snapshot_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);
	join_writer = net_create_writer(&net, net.join_topic, NET_RELIABLE_ALL);
	input_writer = net_create_writer(&net, net.input_topic, NET_BEST_EFFORT);
	dds_entity_t leave_writer = net_create_writer(&net, net.leave_topic, NET_RELIABLE_ALL);
	JoinResponse response = join(100);
	assert(response.status == JOIN_OK && response.player_id == bots + 1);
	arr_ids[0] = response.player_id;
	if (!bots)
	{
		response = join(101);
		assert(response.status == JOIN_OK && response.player_id == 2);
		arr_ids[1] = response.player_id;
	}
	wait_phase(PHASE_COUNTDOWN, 7);
	wait_phase(PHASE_PLAYING, 5);
	if (bots) assert(latest.players[0].flags & PF_BOT);
	int index = arr_ids[0] - 1, start_x = latest.players[index].x;
	arr_keys[0] = KEY_RIGHT | KEY_DASH;
	last_send = 0;
	bool dashed = false;
	dds_time_t deadline = dds_time() + DDS_MSECS(180);
	while (dds_time() < deadline)
	{
		pump();
		if ((latest.players[index].flags & PF_DASHING) &&
		    latest.players[index].x > start_x && latest.players[index].vx == 2 * PLAYER_SPEED)
		{
			dashed = true;
			break;
		}
	}
	assert(dashed);
	arr_keys[0] = 0;
	response = join(999);
	assert(response.status == JOIN_IN_GAME && response.player_id == 0);
	assert(join(100).player_id == arr_ids[0]);
	Leave leave = {.player_id = arr_ids[0], .seq = ++arr_seq[0]};
	assert(dds_write(leave_writer, &leave) == DDS_RETCODE_OK);
	arr_ids[0] = 0;
	wait_phase(PHASE_GAME_OVER, 2);
	assert(latest.players[bots ? 0 : arr_ids[1] - 1].lives == 3);
	wait_phase(PHASE_LOBBY, 7);
	assert(join(999).status == JOIN_OK);
	assert(dds_delete(net.participant) == DDS_RETCODE_OK);
	puts("PASS joins, snapshots, dash, admission, retries, Leave/win and rematch");
	return 0;
}
