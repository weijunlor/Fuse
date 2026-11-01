/* DDS security acceptance driver, also built against the untouched baseline
IDL. Uses real DDS endpoints: a failed handshake must not be confused with
a successful join. FUSE uses the same application-login helpers as the real
client; the separate baseline build retains the lecturer's original messages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dds_security.h"
#ifdef FUSE_PROBE_BASELINE
#include "messages.h"
#else
#include "net.h"
#include "fuse.h"
#include "types.h"
#include "config.h"
#include "app_client.h"
#endif

static dds_entity_t checked(dds_entity_t a_entity)
{
    if (a_entity < 0)
    {
        fprintf(stderr, "Probe DDS error: %s\n", dds_strretcode(-a_entity));
        exit(2);
    }
    return a_entity;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: dds_security_probe participant|deny-writer TOPIC|deny-reader TOPIC|play [seconds]\n");
        return 2;
    }
    dds_entity_t participant = dds_security_create_participant(NET_ROLE_CLIENT);
    if (strcmp(argv[1], "participant") == 0)
    {
        puts("participant=ok");
        dds_delete(participant);
        return 0;
    }
    if (strncmp(argv[1], "deny-", 5) == 0 && argc == 3)
    {
#ifdef FUSE_PROBE_BASELINE
        const dds_topic_descriptor_t *p_desc = &Position_desc;
#else
        const dds_topic_descriptor_t *p_desc = &GameSnapshot_desc;
#endif
        dds_entity_t topic = dds_create_topic(participant, p_desc, argv[2], NULL, NULL);
        dds_entity_t endpoint = topic;
        if (topic >= 0)
            endpoint = strcmp(argv[1], "deny-writer") == 0 ?
                dds_create_writer(participant, topic, NULL, NULL) :
                dds_create_reader(participant, topic, NULL, NULL);
        bool b_denied = endpoint == DDS_RETCODE_NOT_ALLOWED_BY_SECURITY;
        printf("denied=%d topic=%s result=%d\n", b_denied, argv[2], endpoint);
        dds_delete(participant);
        return b_denied ? 0 : 1;
    }
    if (strcmp(argv[1], "play") != 0)
        return 2;
    double d_seconds = argc > 2 ? atof(argv[2]) : 4.0;
    if (d_seconds <= 0 || d_seconds > 60)
        return 2;
#ifndef FUSE_PROBE_BASELINE
    AppClient client_auth;
    if (!app_client_init(&client_auth))
        return 2;
#endif
    dds_entity_t join_topic = checked(dds_create_topic(participant, &JoinRequest_desc, "JoinRequest", NULL, NULL));
    dds_entity_t response_topic = checked(dds_create_topic(participant, &JoinResponse_desc, "JoinResponse", NULL, NULL));
    dds_entity_t input_topic = checked(dds_create_topic(participant, &Input_desc, "Input", NULL, NULL));
#ifdef FUSE_PROBE_BASELINE
    dds_entity_t state_topic = checked(dds_create_topic(participant, &Position_desc, "Position", NULL, NULL));
    dds_entity_t join_writer = checked(dds_create_writer(participant, join_topic, NULL, NULL));
    dds_entity_t response_reader = checked(dds_create_reader(participant, response_topic, NULL, NULL));
    dds_entity_t input_writer = checked(dds_create_writer(participant, input_topic, NULL, NULL));
    dds_entity_t state_reader = checked(dds_create_reader(participant, state_topic, NULL, NULL));
    JoinRequest request = {.b_dummy = true};
    Position state;
#else
    NetTopics net = {.participant = participant};
    dds_entity_t state_topic = checked(dds_create_topic(participant, &GameSnapshot_desc, "GameSnapshot", NULL, NULL));
    dds_entity_t leave_topic = checked(dds_create_topic(participant, &Leave_desc, "Leave", NULL, NULL));
    dds_entity_t join_writer = net_create_writer(&net, join_topic, NET_RELIABLE_ALL);
    dds_entity_t response_reader = net_create_reader(&net, response_topic, NET_RELIABLE_ALL);
    dds_entity_t input_writer = net_create_writer(&net, input_topic, NET_BEST_EFFORT);
    dds_entity_t state_reader = net_create_reader(&net, state_topic, NET_BEST_EFFORT);
    dds_entity_t leave_writer = net_create_writer(&net, leave_topic, NET_RELIABLE_ALL);
    JoinRequest request;
    if (!app_client_request(&client_auth, &request))
        return 2;
    GameSnapshot state;
#endif
    Input input = {0};
    JoinResponse response;
    dds_sample_info_t info;
    int int_id = 0, int_states = 0, int_first_x = -1, int_last_x = -1;
    bool b_moved = false, b_left = false, b_leave_confirmed = false;
    dds_time_t start = dds_time(), last_join = 0, last_input = 0;
    while (dds_time() - start < (dds_duration_t)(d_seconds * 1e9))
    {
        dds_time_t now = dds_time();
        if (int_id == 0 && now - last_join > DDS_SECS(1))
        {
            checked(dds_write(join_writer, &request));
            last_join = now;
        }
        void *p_response = &response;
        if (dds_take(response_reader, &p_response, &info, 1, 1) > 0 && info.valid_data)
        {
#ifdef FUSE_PROBE_BASELINE
            int_id = response.int_player_id;
#else
            if (int_id == 0 && app_client_response(&client_auth, &request, &response))
                int_id = response.player_id;
#endif
        }
        void *p_state = &state;
        if (dds_take(state_reader, &p_state, &info, 1, 1) > 0 && info.valid_data && int_id > 0 && int_id <= 5)
        {
            int_states++;
#ifdef FUSE_PROBE_BASELINE
            if (state.int_player_id == int_id)
                int_last_x = state.int_x;
#else
            int_last_x = state.players[int_id - 1].x;
            if (b_left && !(state.players[int_id - 1].flags & PF_ACTIVE))
            {
                b_leave_confirmed = true;
                break;
            }
#endif
            if (int_last_x >= 0 && int_first_x < 0)
                int_first_x = int_last_x;
            if (int_first_x >= 0 && int_last_x != int_first_x)
                b_moved = true;
        }
        if (int_id > 0 && !b_left && now - last_input > DDS_MSECS(100))
        {
#ifdef FUSE_PROBE_BASELINE
            input.int_player_id = int_id;
            input.ch_direction = int_first_x > 400 ? 'l' : 'r';
#else
            input.player_id = (uint8_t)int_id;
            input.keys = int_first_x > 400 ? KEY_LEFT : KEY_RIGHT;
            if (!app_client_input(&client_auth, &input))
                return 2;
#endif
            checked(dds_write(input_writer, &input));
            last_input = now;
        }
#ifndef FUSE_PROBE_BASELINE
        if (int_id > 0 && !b_left && now - start > (dds_duration_t)((d_seconds - 0.5) * 1e9))
        {
            Leave leave = {.player_id = (uint8_t)int_id};
            if (!app_client_leave(&client_auth, &leave))
                return 2;
            checked(dds_write(leave_writer, &leave));
            dds_wait_for_acks(leave_writer, DDS_MSECS(300));
            b_left = true;
        }
#endif
        dds_sleepfor(DDS_MSECS(2));
    }
    printf("joined=%d id=%d states=%d moved=%d leave_sent=%d leave_confirmed=%d first_x=%d last_x=%d\n",
           int_id > 0, int_id, int_states, b_moved, b_left, b_leave_confirmed, int_first_x, int_last_x);
    dds_delete(participant);
    return int_id > 0 && int_states > 0 && b_moved ? 0 : 1;
}
