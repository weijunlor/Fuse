/* server/net_server.h
Server DDS worker thread: takes inputs, publishes snapshots. */

#ifndef SERVER_NET_SERVER_H
#define SERVER_NET_SERVER_H

/* DDS worker thread (start with pthread_create, never returns). Polls in a
loop: answers join requests, publishes each new GameSnapshot made by
game_tick(), and stores received inputs' held keys with game_set_keys(). */
void *net_server_worker(void *arg);

#endif /* SERVER_NET_SERVER_H */
