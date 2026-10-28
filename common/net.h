/* common/net.h
DDS setup helpers: participant, topics, readers/writers, QoS profiles, waitsets. */

#ifndef COMMON_NET_H
#define COMMON_NET_H

#include <stdbool.h>

#include "dds/dds.h"

/* An array of one message (aka sample in dds terms) will be used. */
#define MAX_SAMPLES 1

// Inputs the server takes per dds_take with tuned QoS (#14): one call empties
// the queue instead of taking one input per pass of the worker loop
#define INPUT_TAKE 8

/* QoS per topic (#14). Cyclone's defaults are a RELIABLE writer and a
BEST_EFFORT reader with KEEP_LAST 1, which together deliver best-effort with a
history of one: a lost or overwritten JoinRequest/JoinResponse is gone.
- NET_RELIABLE_ALL: reliable, keep-all. For join messages: every request and
  response arrives (resent if lost) and none overwrites another.
- NET_BEST_EFFORT: best-effort, keep-last 1 (per key). For Input and
  GameSnapshot: only the newest one matters and a newer one follows within
  100 ms (Input) or one tick (snapshot), so resending an old one is wasted. */
typedef enum
{
    NET_RELIABLE_ALL,
    NET_BEST_EFFORT
} NetQos;

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

// True unless FUSE_QOS=default is set in the environment, which keeps
// Cyclone's default QoS for every reader and writer and takes one input per
// dds_take (the "before" of #14). Server and clients read the same variable.
bool net_qos_tuned(void);

// Create a reader / writer on a topic with the QoS for a_qos (or with the
// default QoS if net_qos_tuned() is false)
dds_entity_t net_create_reader(const NetTopics *a_p_net, dds_entity_t a_topic, NetQos a_qos);
dds_entity_t net_create_writer(const NetTopics *a_p_net, dds_entity_t a_topic, NetQos a_qos);

#endif /* COMMON_NET_H */
