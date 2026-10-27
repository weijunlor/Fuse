/* server/net_server.h
Server DDS worker thread: takes inputs, publishes snapshots. */

#ifndef SERVER_NET_SERVER_H
#define SERVER_NET_SERVER_H

/* DDS worker thread (start with pthread_create, never returns). Polls in a
loop: answers join requests, publishes every active player's Position every
0.1 s, and applies received inputs through game_apply_input(). */
void *net_server_worker(void *arg);

#endif /* SERVER_NET_SERVER_H */
