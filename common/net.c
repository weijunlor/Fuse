/* common/net.c
DDS setup helpers: participant, topics, readers/writers, QoS profiles, waitsets. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "log.h"
#include "fuse.h"

void net_create_topics(NetTopics *a_p_net)
{
    /* ----------------------------------------------------- */
    /* Create participant                                    */
    /* ----------------------------------------------------- */
    a_p_net->participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);

    if (a_p_net->participant < 0)
    {
        LOG_ERROR("Failed to create participant\n");
    }

    /* ----------------------------------------------------- */
    /* Create topics                                         */
    /* ----------------------------------------------------- */
    a_p_net->join_topic = dds_create_topic(a_p_net->participant,
        &JoinRequest_desc, "JoinRequest", NULL, NULL);

    a_p_net->response_topic = dds_create_topic(a_p_net->participant,
        &JoinResponse_desc, "JoinResponse", NULL, NULL);

    a_p_net->input_topic = dds_create_topic(a_p_net->participant,
        &Input_desc, "Input", NULL, NULL);

    a_p_net->snapshot_topic = dds_create_topic(a_p_net->participant,
        &GameSnapshot_desc, "GameSnapshot", NULL, NULL);
}

bool net_qos_tuned(void)
{
    static int int_tuned = -1;   // -1: environment not read yet
    if (int_tuned < 0)
    {
        const char *str_qos = getenv("FUSE_QOS");
        int_tuned = !(str_qos != NULL && strcmp(str_qos, "default") == 0);
        LOG_INFO("DDS QoS: %s\n", int_tuned ? "per topic (#14)" : "Cyclone defaults");
    }
    return int_tuned;
}

// QoS object for a_qos, or NULL (default QoS) when not tuned. Free with
// dds_delete_qos (accepts NULL).
static dds_qos_t *make_qos(NetQos a_qos)
{
    if (!net_qos_tuned())
        return NULL;

    dds_qos_t *p_qos = dds_create_qos();
    if (a_qos == NET_RELIABLE_ALL)
    {
        // A writer may block up to 100 ms if the history is full (it never
        // is: keep-all with no resource limits)
        dds_qset_reliability(p_qos, DDS_RELIABILITY_RELIABLE, DDS_MSECS(100));
        dds_qset_history(p_qos, DDS_HISTORY_KEEP_ALL, 0);
    }
    else
    {
        dds_qset_reliability(p_qos, DDS_RELIABILITY_BEST_EFFORT, 0);
        dds_qset_history(p_qos, DDS_HISTORY_KEEP_LAST, 1);
    }
    return p_qos;
}

dds_entity_t net_create_reader(const NetTopics *a_p_net, dds_entity_t a_topic, NetQos a_qos)
{
    dds_qos_t *p_qos = make_qos(a_qos);
    dds_entity_t reader = dds_create_reader(a_p_net->participant, a_topic, p_qos, NULL);
    dds_delete_qos(p_qos);
    if (reader < 0)
        DDS_FATAL("dds_create_reader: %s\n", dds_strretcode(-reader));
    return reader;
}

dds_entity_t net_create_writer(const NetTopics *a_p_net, dds_entity_t a_topic, NetQos a_qos)
{
    dds_qos_t *p_qos = make_qos(a_qos);
    dds_entity_t writer = dds_create_writer(a_p_net->participant, a_topic, p_qos, NULL);
    dds_delete_qos(p_qos);
    if (writer < 0)
        DDS_FATAL("dds_create_writer: %s\n", dds_strretcode(-writer));
    return writer;
}
