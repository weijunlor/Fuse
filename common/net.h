/* common/net.h
DDS setup helpers: participant, topics, readers/writers, QoS profiles, waitsets. */

#ifndef COMMON_NET_H
#define COMMON_NET_H

#include "dds/dds.h"

/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1

// The participant and the topics used by both server and client
// (message types in idl/fuse.idl)
typedef struct
{
    dds_entity_t participant;
    dds_entity_t join_topic;        // JoinRequest, client -> server
    dds_entity_t response_topic;    // JoinResponse, server -> clients
    dds_entity_t input_topic;       // Input (held keys), client -> server
    dds_entity_t snapshot_topic;    // GameSnapshot, server -> clients, once per published tick
} NetTopics;

// Create the participant on the default domain and all topics (default QoS)
void net_create_topics(NetTopics *a_p_net);

#endif /* COMMON_NET_H */
