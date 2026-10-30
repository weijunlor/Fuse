/* bench/dds_joiner.c
Headless client for join tests (#14): joins the game exactly as the real
client does (same topics and QoS from common/net.c, FUSE_QOS and
FUSE_JOIN_NONCE honoured, a JoinRequest every 1 s until its JoinResponse
arrives), then stays connected and
watches the snapshots. bench/join_loss.sh runs several of these while the
loopback interface drops packets.

Usage: dds_joiner <seconds>
Prints one line when it ends:
  joined=<0|1> id=<player id> join_ms=<first request to accepted response>
  requests=<JoinRequests sent>
  active=<active players in the last snapshot> snapshots=<snapshots received> */

#include <stdio.h>
#include <stdlib.h>

#include "net.h"
#include "fuse.h"
#include "types.h"
#include "config.h"
#include "app_client.h"

int main(int argc, char **argv)
{
    double d_seconds = argc > 1 ? atof(argv[1]) : 10.0;
    AppClient client_auth;
    if (!app_client_init(&client_auth))
        return 2;

    // Same entities and QoS as client/net_client.c
    NetTopics net;
    net_create_topics(&net, NET_ROLE_CLIENT);
    dds_entity_t join_writer = net_create_writer(&net, net.join_topic, NET_RELIABLE_ALL);
    dds_entity_t response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
    dds_entity_t snapshot_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);

    void *arr_p_responses[JOIN_TAKE];
    dds_sample_info_t infos[JOIN_TAKE];
    for (int i = 0; i < JOIN_TAKE; i++)
        arr_p_responses[i] = JoinResponse__alloc();
    void *p_snapshot = GameSnapshot__alloc();
    dds_sample_info_t info;

    // As the client: one nonce, sent in every request
    JoinRequest request = {0};
    bool b_join_nonce = client_auth.b_enabled || net_join_nonce();
    if (!app_client_request(&client_auth, &request))
        return 2;
    int int_id = 0, int_requests = 0, int_snapshots = 0, int_active = -1;
    dds_time_t start = dds_time();
    dds_time_t end = start + (dds_duration_t)(d_seconds * 1e9);
    // As the client: the first request goes out 1 s after start (by then
    // discovery has matched the server's reader)
    dds_time_t last_request = start, first_request = 0, joined_at = 0;

    while (dds_time() < end)
    {
        dds_time_t now = dds_time();
        if (int_id == 0)
        {
            // As the client: a request every 1 s until answered
            if (now - last_request > DDS_SECS(1))
            {
                dds_write(join_writer, &request);
                if (int_requests++ == 0)
                    first_request = now;
                last_request = now;
            }
            // As the client: the response with our nonce (any response
            // with FUSE_JOIN_NONCE=off) is taken as ours
            int rc = dds_take(response_reader, arr_p_responses, infos, JOIN_TAKE, JOIN_TAKE);
            for (int i = 0; i < rc && int_id == 0; i++)
            {
                const JoinResponse *p_r = arr_p_responses[i];
                if (!infos[i].valid_data || (b_join_nonce && p_r->nonce != request.nonce))
                    continue;
                if (app_client_response(&client_auth, &request, p_r))
                {
                    int_id = p_r->player_id;
                    joined_at = dds_time();
                    // Took a response meant for another client before even
                    // asking (the baseline join race): count it as 0 ms
                    if (int_requests == 0)
                        first_request = joined_at;
                }
            }
        }

        // Count active players in the newest snapshot
        if (dds_take(snapshot_reader, &p_snapshot, &info, 1, 1) > 0 && info.valid_data)
        {
            const GameSnapshot *p_s = p_snapshot;
            int_snapshots++;
            int_active = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                int_active += (p_s->players[i].flags & PF_ACTIVE) != 0;
        }
        dds_sleepfor(DDS_MSECS(2));
    }

    printf("joined=%d id=%d join_ms=%.0f requests=%d active=%d snapshots=%d\n",
           int_id != 0, int_id, int_id ? (joined_at - first_request) / 1e6 : -1.0,
           int_requests, int_active, int_snapshots);
    crypto_clear(&client_auth, sizeof client_auth);
    dds_delete(net.participant);
    return 0;
}
