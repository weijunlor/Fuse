/** @file common/net.h
 * @brief DDS setup helpers shared by server and client: participant, topics,
 * readers/writers with per-topic QoS (#14), and the join-nonce / leave
 * switches.
 * @details The environment switches (FUSE_QOS, FUSE_JOIN_NONCE, FUSE_LEAVE)
 * are read once, thread-safely, on first use.
 */

#ifndef COMMON_NET_H
#define COMMON_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "dds/dds.h"
#include "dds_security.h"

/** @brief Samples per dds_take for snapshots and other single-sample reads. */
#define MAX_SAMPLES 1

/** @brief Inputs the server takes per dds_take with tuned QoS (#14).
 * @details One call empties the queue instead of taking one input per pass of
 * the worker loop. */
#define INPUT_TAKE 8

/** @brief Join messages taken per dds_take (requests by the server, responses
 * by a client).
 * @details With the join nonce, a client also receives every other client's
 * responses, and one call empties the queue. */
#define JOIN_TAKE 8

/** @brief QoS profile per topic (#14).
 * @details Cyclone's defaults are a RELIABLE writer and a BEST_EFFORT reader
 * with KEEP_LAST 1, which together deliver best-effort with a history of one:
 * a lost or overwritten JoinRequest/JoinResponse is gone.
 */
typedef enum
{
    /** Reliable, keep-all. For join messages and Leave: every request and
     * response arrives (resent if lost) and none overwrites another. */
    NET_RELIABLE_ALL,
    /** Best-effort, keep-last 1 (per key). For Input and GameSnapshot: only
     * the newest one matters and a newer one follows within 100 ms (Input) or
     * one tick (snapshot), so resending an old one is wasted. */
    NET_BEST_EFFORT
} NetQos;

/** @brief The participant and the topics used by both server and client
 * (message types in idl/fuse.idl). */
typedef struct
{
    dds_entity_t participant;       ///< Secure (or plaintext) domain participant
    dds_entity_t join_topic;        ///< JoinRequest, client -> server
    dds_entity_t response_topic;    ///< JoinResponse, server -> clients
    dds_entity_t input_topic;       ///< Input (held keys), client -> server
    dds_entity_t snapshot_topic;    ///< GameSnapshot, server -> clients, once per published tick
    dds_entity_t leave_topic;       ///< Leave, client -> server when the player quits
} NetTopics;

/** @brief Create the role's participant on domain 0 and all FUSE topics.
 * @details The participant is secure by default; FUSE_DDS_SECURITY=off
 * explicitly selects plaintext for comparisons.
 * @param[out] a_p_net Receives the participant and topic handles.
 * @param a_role Server or client identity (selects credentials).
 * @note Exits the process if a topic cannot be created.
 */
void net_create_topics(NetTopics *a_p_net, NetRole a_role);

/** @brief Whether per-topic QoS (#14) is on.
 * @details True unless FUSE_QOS=default is set in the environment, which
 * keeps Cyclone's default QoS for every reader and writer and takes one input
 * per dds_take (the "before" of #14). Server and clients read the same
 * variable.
 * @return True for tuned QoS.
 */
bool net_qos_tuned(void);

/** @brief Create a reader on a topic with the QoS for a profile.
 * @param a_p_net Participant and topics from net_create_topics().
 * @param a_topic Topic handle.
 * @param a_qos Profile; ignored (Cyclone defaults) if net_qos_tuned() is false.
 * @return Reader handle. Exits the process on failure.
 */
dds_entity_t net_create_reader(const NetTopics *a_p_net, dds_entity_t a_topic, NetQos a_qos);

/** @brief Create a writer on a topic with the QoS for a profile.
 * @param a_p_net Participant and topics from net_create_topics().
 * @param a_topic Topic handle.
 * @param a_qos Profile; ignored (Cyclone defaults) if net_qos_tuned() is false.
 * @return Writer handle. Exits the process on failure.
 */
dds_entity_t net_create_writer(const NetTopics *a_p_net, dds_entity_t a_topic, NetQos a_qos);

/** @brief Whether the join nonce is on (FUSE_JOIN_NONCE).
 * @details Every JoinResponse goes to every client, so in the baseline each
 * waiting client took the first response it saw: two clients joining together
 * got the same player ID, and a client whose response was lost asked again and
 * got a second slot while the first stayed filled ("ghost"). With the nonce:
 * - the client picks a random non-zero nonce once and sends it in every
 *   JoinRequest; it accepts only a JoinResponse carrying the same nonce;
 * - the server copies the nonce into the response and remembers it with the slot;
 *   a repeated request with that nonce gets the same slot again, not a new one.
 * FUSE_JOIN_NONCE=off in the environment restores the baseline behaviour (the
 * "before"): clients send nonce 0 and take any response, and the server gives
 * every request a new slot. Server and clients read the same variable.
 * @return True unless FUSE_JOIN_NONCE=off.
 */
bool net_join_nonce(void);

/** @brief A random non-zero 64-bit nonce from the kernel's random source (getrandom).
 * @return The nonce (never 0, which means "no nonce"). Exits if getrandom fails.
 */
uint64_t net_random_nonce(void);

/** @brief Whether Leave messages and the player timeout are on (FUSE_LEAVE).
 * @details The baseline never freed a player slot: a client that
 * quit or crashed stayed in the game (frozen) until the server restarted, and
 * after 5 clients nobody else could join.
 * - A client that quits sends a Leave (reliable) and the server frees its slot.
 * - A client that crashes sends nothing, so the server frees the slot of any
 *   player that sent no Input for --player-timeout seconds (config.h).
 * - A client whose slot was freed (it was cut off for that long) sees itself
 *   missing from the snapshots and joins again.
 * FUSE_LEAVE=off in the environment restores the baseline (no Leave sent, none
 * handled, no timeout, no rejoin). Server and clients read the same variable.
 * @return True unless FUSE_LEAVE=off.
 */
bool net_leave(void);

#endif /* COMMON_NET_H */
