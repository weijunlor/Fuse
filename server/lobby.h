/* server/lobby.h
Player slots: join (nonce), leave, liveliness timeout. */

#ifndef SERVER_LOBBY_H
#define SERVER_LOBBY_H

/* Join handshake between the two server threads:
1. DDS worker receives a JoinRequest and calls lobby_signal_join().
2. Main thread's lobby_poll() sees the flag, picks a free slot and a random
   non-colliding position, and stores the new player ID.
3. DDS worker collects the ID with lobby_take_new_player_id() and sends it
   in a JoinResponse. All flags are guarded by the mutex in game.h. */

// Called by the DDS worker when a join request arrives
void lobby_signal_join(void);

// Called by the DDS worker. Returns -1 if no new player ID is waiting,
// 0 if the game was full, or the new player ID (1~MAX_PLAYERS); the stored
// ID is reset to -1 once taken.
int lobby_take_new_player_id(void);

// Called repeatedly by the main thread: handles a pending join request
void lobby_poll(void);

#endif /* SERVER_LOBBY_H */
