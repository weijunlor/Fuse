/* server/net_server.h
Server DDS worker thread: takes inputs, publishes snapshots. */

#ifndef SERVER_NET_SERVER_H
#define SERVER_NET_SERVER_H

#include <stdbool.h>

/* #2: true (default) = the main thread sleeps until the next tick and the
worker blocks in a DDS waitset until data arrives or net_server_wake() is
called. false (FUSE_SERVER_WAIT=spin) = both threads busy-poll, as before #2
(the baseline's loop), kept so the change can be measured. */
extern bool b_server_wait;

/* #1: false (default) = the worker copies the snapshot under the mutex and
publishes it after unlocking. true (FUSE_WRITE_UNDER_LOCK=on) = dds_write()
runs while the mutex is held, as the baseline's Position writes did, so the
main thread waits for the network write. */
extern bool b_write_under_lock;

/* Read FUSE_SERVER_WAIT and FUSE_WRITE_UNDER_LOCK from the environment and
create the guard condition used by net_server_wake(). Call once from main()
before starting the worker. */
void net_server_init(void);

/* Wake the worker so it publishes the snapshot and join answers the main
thread has just made (sets a DDS guard condition attached to the worker's
waitset). Does nothing in spin mode, where the worker never blocks. */
void net_server_wake(void);

/* DDS worker thread (start with pthread_create, never returns). Waits for
network data or net_server_wake(), answers join requests, publishes each new
GameSnapshot made by game_tick(), and queues copied Input/Leave samples for authenticated
consumption by the main thread (or validates directly in mutex mode). */
void *net_server_worker(void *arg);

#endif /* SERVER_NET_SERVER_H */
