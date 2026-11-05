/** @file server/lobby.h
 * @brief Server player admission, leave, heartbeat timeout and bot-slot API.
 * @details Shared module: slot management integrates the team's authentication
 * and SPSC/mutex handoffs. Queue admission does not authenticate a message.
 *
 * Join handshake between the two server threads:
 * 1. DDS worker receives a JoinRequest and queues the bounded sample with
 *    lobby_signal_join().
 * 2. Main thread's lobby_poll() authenticates every queued request: a valid
 *    nonce that already holds a slot gets that slot again (its response was
 *    lost and the client asked again); otherwise it picks a free slot and a
 *    random non-colliding position in the lobby, or answers "full" / "in game".
 *    Retries for an existing nonce remain valid during a match.
 * 3. DDS worker collects the answers with lobby_take_result() and sends each
 *    in a JoinResponse carrying the request's nonce.
 *
 * SPSC mode copies requests/results across bounded rings; slot state is owned
 * by the main thread. The mutex comparison retains the original queues.
 *
 * Leaving (net_leave() in common/net.h): the DDS worker submits copied Leave
 * samples; lobby_poll() authenticates them and frees those slots. The main
 * thread also calls lobby_check_timeouts() once per tick, which frees the slot
 * of any player that sent no Input for int_player_timeout_s seconds. A freed
 * slot erases its session key and can be given to the next client that joins.
 * Authentication retains the admitted nonce in its replay ledger for this run.
 *
 * The baseline used one flag and one stored ID instead of the two queues, so
 * requests arriving together were merged into one, and the server could not
 * tell a repeated request from a new client (see net_join_nonce() in
 * common/net.h).
 */

#ifndef SERVER_LOBBY_H
#define SERVER_LOBBY_H

#include <stdbool.h>
#include <stdint.h>
#include "fuse.h"

/** @brief Join requests (and answers, and Leaves) that can wait between two
 * passes of the threads.
 * @details More than that are dropped, and the client asks again 1 s later. */
#define JOIN_QUEUE 16
/** @brief Inputs that can wait in the SPSC input ring for the main thread. */
#define INPUT_QUEUE 256

/** @brief One answer to a JoinRequest, for the DDS worker to send.
 * @details status is JOIN_OK, JOIN_FULL, JOIN_IN_GAME, JOIN_BAD_LOGIN or
 * JOIN_RETRY (types.h). */
typedef JoinResponse JoinResult;

/** @brief Copy a join request from the DDS worker into the pending queue.
 * @details Called by the DDS worker when a join request arrives; the sample
 * is copied before DDS reuses its storage.
 * @param a_request Request to copy; caller retains ownership.
 * @return True if queued, false when full (the client retries later).
 */
bool lobby_signal_join(const JoinRequest *a_request);

/** @brief Consume the oldest join answer for the DDS worker to publish.
 * @note Called by the DDS worker.
 * @param[out] a_p_out Receives an answer on success.
 * @return True when copied; false when there is no answer.
 */
bool lobby_take_result(JoinResult *a_p_out);

/** @brief Seconds without Input after which a player's slot is freed (0 = never).
 * @details Set by the server's --player-timeout option before starting. */
extern int int_player_timeout_s;

/** @brief Validate an input and update keys/heartbeat while holding mutex.
 * @details Validates and applies under the same mutex as slot
 * admission/removal: no unauthenticated sample may move a player, refresh its
 * timeout, or free it.
 * @param a_input Message to authenticate against the current slot session.
 * @return True when accepted; false for invalid, stale or rejected input.
 */
bool lobby_receive_input(const Input *a_input);

/** @brief Validate a leave and schedule its slot removal under mutex.
 * @param a_leave Leave message to authenticate.
 * @return True when accepted for removal; false if invalid or the queue is full.
 */
bool lobby_receive_leave(const Leave *a_leave);

/** @brief Submit copied input through the selected thread handoff.
 * @details Network entry point: SPSC mode copies into a bounded ring, and the
 * MAIN thread authenticates on consumption, so a true result means queued,
 * not yet authenticated. Full queues return false without changing auth
 * state. Input repeats recover dropped updates; a dropped Leave is recovered
 * by timeout. Mutex comparison mode validates/applies immediately as before.
 * @param a_input Message received by the DDS worker.
 * @return In SPSC mode, true means queued; in mutex mode, true means validated
 * and applied. False means the queue was full or validation failed.
 */
bool lobby_submit_input(const Input *a_input);

/** @brief Submit a copied leave through the selected thread handoff.
 * @param a_leave Message received by the DDS worker.
 * @return True if queued in SPSC mode or validated for removal in mutex mode;
 * false on a full queue or failed validation.
 */
bool lobby_submit_leave(const Leave *a_leave);

/** @brief Process queued inputs, leaves and join requests on the main thread.
 * @details Leaves free slots before joins are admitted. New players are accepted
 * only in the lobby; retries for an existing session can be answered in a match.
 * @note Acquires mutex as needed. SPSC input/leave samples are validated here.
 */
void lobby_poll(void);

/** @brief Remove silent human players and clear their session state.
 * @param a_u64_now_ns Current monotonic time from timing_now_ns().
 * @note Main thread, once per tick. Acquires mutex; skips bots and does nothing
 * if timeouts or Leave handling have been disabled.
 */
void lobby_check_timeouts(uint64_t a_u64_now_ns);

/** @brief Reserve and initialise server-controlled player slots during startup.
 * @param a_int_count Bot count in 0..MAX_PLAYERS-1.
 * @return True when all requested bots were added; false on invalid count,
 * insufficient capacity or admission failure.
 */
bool lobby_add_bots(int a_int_count);

#endif /* SERVER_LOBBY_H */
