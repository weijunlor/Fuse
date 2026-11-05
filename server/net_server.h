/** @file server/net_server.h
 * @brief Server network worker and wake API (#1/#2/#12/#14 integration).
 * @details Defaults to waiting for work and publishing snapshots outside the
 * state mutex; comparison options retain the earlier busy/locked paths.
 */

#ifndef SERVER_NET_SERVER_H
#define SERVER_NET_SERVER_H

#include <stdbool.h>

/** @brief Server wait mode (#2).
 * @details True (default) = the main thread sleeps until the next tick and
 * the worker blocks in a DDS waitset until data arrives or net_server_wake()
 * is called. False (FUSE_SERVER_WAIT=spin or --wait busy) = both threads
 * busy-poll, as before #2 (the baseline's loop), kept so the change can be
 * measured.
 */
extern bool b_server_wait;

/** @brief Snapshot write mode (#1).
 * @details False (default) = the worker copies the snapshot (under the mutex
 * in mutex handoff mode) and publishes it after unlocking. True
 * (FUSE_WRITE_UNDER_LOCK=on or --publish locked) = dds_write() runs while the
 * mutex is held, as the baseline's Position writes did, so the main thread
 * waits for the network write.
 */
extern bool b_write_under_lock;

/** @brief Create the guard condition used to wake the DDS worker.
 * @pre Call once after selecting options and before starting the worker.
 */
void net_server_init(void);

/** @brief Wake the worker to publish snapshots and queued join answers.
 * @note Sets a thread-safe guard condition; does nothing in spin mode.
 */
void net_server_wake(void);

/** @brief Run the server's DDS read/write loop.
 * @param arg Unused pthread argument; pass NULL.
 * @return Does not return during normal operation.
 * @details Handles joins, copies Input/Leave samples and publishes completed
 * snapshots. Wait mode blocks for reader activity or net_server_wake().
 * Queue authentication is performed by the main thread in SPSC mode.
 */
void *net_server_worker(void *arg);

#endif /* SERVER_NET_SERVER_H */
