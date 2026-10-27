/* common/net.c
DDS setup helpers: participant, topics, readers/writers, QoS profiles, waitsets. */

#include <stdio.h>

#include "net.h"
#include "messages.h"

void net_create_topics(NetTopics *a_p_net)
{
    /* ----------------------------------------------------- */
    /* Create participant                                    */
    /* ----------------------------------------------------- */
    a_p_net->participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);

    if (a_p_net->participant < 0)
    {
        printf("Failed to create participant\n");
    }

    /* ----------------------------------------------------- */
    /* Create topics                                         */
    /* ----------------------------------------------------- */
    a_p_net->join_topic = dds_create_topic(a_p_net->participant,
        &JoinRequest_desc, "JoinRequest", NULL, NULL);

    a_p_net->response_topic = dds_create_topic(a_p_net->participant,
        &JoinResponse_desc, "JoinResponse", NULL, NULL);

    a_p_net->position_topic = dds_create_topic(a_p_net->participant,
        &Position_desc, "Position", NULL, NULL);

    a_p_net->input_topic = dds_create_topic(a_p_net->participant,
        &Input_desc, "Input", NULL, NULL);
}
