/** @file client/net_client.h
 * @brief Client session lifecycle and copied network-frame API.
 * @details Shared worker API. The lobby uses prepare/start, stop/join/finish
 * for repeatable sessions; authentication and handoff algorithms are team work.
 */

#ifndef CLIENT_NET_CLIENT_H
#define CLIENT_NET_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "types.h"
#include "fuse.h"

/** @brief Connection status published with the worker-owned player ID.
 * @details The worker-owned ID and snapshot are published TOGETHER. The
 * renderer never accesses the worker's live state (#4), and uses one complete
 * copy per frame.
 */
typedef enum
{
    CLIENT_JOINING,       /**< Waiting for a server response. */
    CLIENT_JOINED,        /**< A player slot has been assigned. */
    CLIENT_WAITING_MATCH, /**< Retrying until the server returns to its lobby. */
    CLIENT_FULL,          /**< Retrying while all player slots are occupied. */
    CLIENT_REJECTED       /**< Login rejected; menu can retry with a fresh session. */
} ClientConnection;

/** @brief Coherent copy shared by the main-thread menu and renderer. */
typedef struct
{
    int int_player_id;           /**< Assigned 1-based ID; -1 until joined. */
    ClientConnection connection; /**< Join status for menu feedback. */
    uint64_t u64_received_ns;     /**< Snapshot arrival in monotonic nanoseconds. */
    GameSnapshot snapshot;       /**< Latest received server state. */
} ClientFrame;

/** @brief Copy the latest published worker frame on the main thread.
 * @details Main thread only. Initialise the caller's frame ID to -1; on false
 * the caller keeps its previous frame.
 * @param[in,out] a_frame Receives a frame on success; otherwise unchanged.
 * @return True if copied; false if no new SPSC frame or a publication collision.
 * Mutex mode always copies its current frame and returns true.
 * @pre Prepare the session first; use exactly one main-thread consumer.
 */
bool net_client_read_frame(ClientFrame *a_frame);

/** @brief Publish the main thread's held-key bitmask and wake the worker on change.
 * @details Main -> DDS worker: an atomic held-key value, latest state wins.
 * @param a_u8_keys Current KEY_* bits, or 0 to release movement.
 * @pre The session has been prepared and has not been finished.
 */
void net_client_set_keys(uint8_t a_u8_keys);

/** @brief Input send policy (#13).
 * @details True (default) sends an Input only when the held keys change, plus
 * a repeat every 100 ms; false sends one every 10 ms while keys are held (the
 * baseline rate). Set before starting the worker (--input-send in main.c).
 */
extern bool b_input_on_change;

/** @brief Worker wait mode (#2): true = waitset (default), false =
 * FUSE_CLIENT_WAIT=spin polling for comparison. Set by net_client_init(). */
extern bool b_client_wait;
/** @brief Initialise the configured wait mode and wake guard during preparation. */
void net_client_init(void);
/** @brief Wake a prepared worker after input changes or shutdown. */
void net_client_wake(void);

/** @brief Prepare credentials, handoff state and wake guard for a new session.
 * @return True when ready for pthread_create(); false on recoverable setup failure.
 * @pre Any previous worker has been joined and its session finished.
 */
bool net_client_prepare(void);

/** @brief Delete the wake guard and clear credentials after a session.
 * @details Releases the session's library-owned wake guard and clears
 * credentials before a rejoin.
 * @pre Main thread only, after pthread_join() or a failed prepare/start attempt.
 */
void net_client_finish(void);

/** @brief DDS worker thread: join the server, send signed input and publish
 * copied snapshots.
 * @param arg Unused pthread argument; pass NULL.
 * @return NULL after shutdown or a rejected login.
 * @details Waits for DDS data, changed keys, shutdown or the next timed
 * message, and runs until net_client_stop() is called. Shutdown sends Leave for an admitted slot when enabled. A rejected
 * login is published to the menu; it does not bypass authentication.
 */
void *net_client_worker(void *arg);

/** @brief Request worker shutdown; the worker performs the Leave exchange.
 * @details The worker sends a Leave (if joined, and FUSE_LEAVE is not off),
 * waits up to 0.5 s for the server to acknowledge it, closes DDS and returns.
 * Called by the main thread on Leave or window close. (The baseline exited
 * without telling the server.)
 * @note Follow with pthread_join() and net_client_finish() before rejoining.
 */
void net_client_stop(void);

#endif /* CLIENT_NET_CLIENT_H */
