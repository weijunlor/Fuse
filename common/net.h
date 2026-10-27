/* common/net.h
DDS setup helpers: participant, topics, readers/writers, QoS profiles, waitsets. */

#ifndef COMMON_NET_H
#define COMMON_NET_H

#include "dds/dds.h"

/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1

// The participant and the four topics used by both server and client
typedef struct
{
    dds_entity_t participant;
    dds_entity_t join_topic;
    dds_entity_t response_topic;
    dds_entity_t position_topic;
    dds_entity_t input_topic;
} NetTopics;

// Create the participant on the default domain and all topics (default QoS)
void net_create_topics(NetTopics *a_p_net);

#endif /* COMMON_NET_H */
