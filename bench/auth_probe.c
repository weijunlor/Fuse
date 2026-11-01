/* Attack/regression driver against the real DDS server, using wire samples. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_client.h"
#include "net.h"
#include "types.h"
#include "config.h"

static NetTopics net;
static dds_entity_t join_writer, response_reader, input_writer, leave_writer, state_reader;
static GameSnapshot state;
static bool b_have_state;
static int int_checks;
#define CHECK(condition) do { int_checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL wire check %d: %s\n", int_checks, #condition); exit(1); } } while (0)

static void observe(int a_ms)
{
    dds_time_t end = dds_time() + DDS_MSECS(a_ms);
    do
    {
        void *p_state = &state;
        dds_sample_info_t info;
        int rc = dds_take(state_reader, &p_state, &info, 1, 1);
        CHECK(rc >= 0);
        if (rc > 0 && info.valid_data) b_have_state = true;
        dds_sleepfor(DDS_MSECS(5));
    } while (dds_time() < end);
}

static JoinResponse join(const JoinRequest *a_request)
{
    dds_time_t end = dds_time() + DDS_SECS(5), last_sent = 0;
    while (dds_time() < end)
    {
        if (dds_time() - last_sent > DDS_MSECS(300))
        {
            CHECK(dds_write(join_writer, a_request) == DDS_RETCODE_OK);
            last_sent = dds_time();
        }
        JoinResponse response;
        void *p_response = &response;
        dds_sample_info_t info;
        int rc = dds_take(response_reader, &p_response, &info, 1, 1);
        CHECK(rc >= 0);
        if (rc > 0 && info.valid_data && response.nonce == a_request->nonce)
            return response;
        observe(10);
    }
    CHECK(false && "join response timed out");
    return (JoinResponse){0};
}

int main(void)
{
    AppClient client;
    CHECK(app_client_init(&client));
    CHECK(client.b_enabled);
    net_create_topics(&net, NET_ROLE_CLIENT);
    join_writer = net_create_writer(&net, net.join_topic, NET_RELIABLE_ALL);
    response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
    input_writer = net_create_writer(&net, net.input_topic, NET_BEST_EFFORT);
    leave_writer = net_create_writer(&net, net.leave_topic, NET_RELIABLE_ALL);
    state_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);
    JoinRequest request;
    CHECK(app_client_request(&client, &request));
    JoinRequest bad = request; bad.proof[0] ^= 1;
    CHECK(join(&bad).status == JOIN_BAD_LOGIN);
    JoinResponse response = join(&request);
    CHECK(app_client_response(&client, &request, &response));
    int int_id = response.player_id;
    JoinResponse retry = join(&request);
    CHECK(retry.status == JOIN_OK && retry.player_id == int_id &&
        crypto_equal(response.session_key, retry.session_key, CRYPTO_SESSION_SIZE));
    observe(150); CHECK(b_have_state && (state.players[int_id - 1].flags & PF_ACTIVE));
    int int_x = state.players[int_id - 1].x;
    Input input = {.player_id = (uint8_t)int_id, .keys = int_x > WIDTH / 2 ? KEY_LEFT : KEY_RIGHT};
    CHECK(app_client_input(&client, &input));
    Input old_input = input;
    // Redundant sends tolerate best-effort delivery; replays themselves are rejected.
    for (int i = 0; i < 3; i++) { dds_write(input_writer, &input); observe(20); }
    observe(300); CHECK(state.players[int_id - 1].x != int_x);
    input.keys = 0; CHECK(app_client_input(&client, &input));
    for (int i = 0; i < 3; i++) { dds_write(input_writer, &input); observe(20); }
    observe(100); int_x = state.players[int_id - 1].x;
    for (int i = 0; i < 5; i++) { dds_write(input_writer, &old_input); observe(40); }
    CHECK(state.players[int_id - 1].x == int_x); // replayed movement ignored
    Input forged = old_input; forged.seq = UINT32_MAX;
    for (int i = 0; i < 5; i++) { dds_write(input_writer, &forged); observe(40); }
    CHECK(state.players[int_id - 1].x == int_x); // altered message ignored
    forged.player_id = 255; dds_write(input_writer, &forged);
    Leave leave = {.player_id = (uint8_t)int_id, .seq = UINT32_MAX};
    CHECK(dds_write(leave_writer, &leave) == DDS_RETCODE_OK);
    observe(150); CHECK(state.players[int_id - 1].flags & PF_ACTIVE);
    input.keys = old_input.keys; CHECK(app_client_input(&client, &input));
    for (int i = 0; i < 3; i++) { dds_write(input_writer, &input); observe(20); }
    observe(150); CHECK(state.players[int_id - 1].x != int_x); // bad high seq did not lock out client
    CHECK(app_client_leave(&client, &leave));
    CHECK(dds_write(leave_writer, &leave) == DDS_RETCODE_OK);
    observe(200); CHECK(!(state.players[int_id - 1].flags & PF_ACTIVE));
    CHECK(join(&request).status == JOIN_BAD_LOGIN); // recorded login cannot resurrect a session
    uint8_t arr_old_key[16]; memcpy(arr_old_key, client.arr_session_key, 16);
    CHECK(app_client_request(&client, &request)); response = join(&request);
    CHECK(app_client_response(&client, &request, &response));
    CHECK(!crypto_equal(arr_old_key, client.arr_session_key, 16));
    CHECK(dds_write(leave_writer, &leave) == DDS_RETCODE_OK); observe(100);
    CHECK(state.players[response.player_id - 1].flags & PF_ACTIVE); // old-session Leave ignored
    leave.player_id = response.player_id;
    CHECK(app_client_leave(&client, &leave));
    CHECK(dds_write(leave_writer, &leave) == DDS_RETCODE_OK); observe(100);
    CHECK(!(state.players[response.player_id - 1].flags & PF_ACTIVE));
    crypto_clear(&client, sizeof client);
    dds_delete(net.participant);
    puts("PASS wire: login, retry, movement, tampering, replay, forged Leave, logout, fresh session, old-key rejection");
    return 0;
}
