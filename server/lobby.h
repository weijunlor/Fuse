/* server/lobby.h
Player slots: join (nonce), leave, liveliness timeout. */

#ifndef SERVER_LOBBY_H
#define SERVER_LOBBY_H

#include <stdbool.h>
#include <stdint.h>

/* Join handshake between the two server threads:
1. DDS worker receives a JoinRequest and queues its nonce with
   lobby_signal_join().
2. Main thread's lobby_poll() handles every queued request: a nonce that
   already holds a slot gets that slot again (its response was lost and the
   client asked again); otherwise it picks a free slot and a random
   non-colliding position in the lobby, or answers "full" / "in game".
   Retries for an existing nonce remain valid during a match.
3. DDS worker collects the answers with lobby_take_result() and sends each in
   a JoinResponse carrying the request's nonce.
Queues and slot nonces are guarded by the mutex in game.h.

Leaving (net_leave() in common/net.h): the DDS worker queues the player ID of
each Leave with lobby_signal_leave(); lobby_poll() frees those slots. The main
thread also calls lobby_check_timeouts() once per tick, which frees the slot
of any player that sent no Input for int_player_timeout_s seconds. A freed
slot forgets its nonce and can be given to the next client that joins.

The baseline used one flag and one stored ID instead of the two queues, so
requests arriving together were merged into one, and the server could not tell
a repeated request from a new client (see net_join_nonce() in common/net.h). */

// Requests (and answers) that can wait between two passes of the threads;
// more than that are dropped, and the client asks again 1 s later
#define JOIN_QUEUE 16

// One answer to a JoinRequest, for the DDS worker to send
typedef struct
{
	uint64_t u64_nonce;    // copied from the request
	uint8_t u8_status;     // JOIN_OK, JOIN_FULL or JOIN_IN_GAME (types.h)
	uint8_t u8_player_id;  // 1..MAX_PLAYERS if JOIN_OK, else 0
} JoinResult;

// Called by the DDS worker when a join request arrives (nonce 0 = the client
// sent none). Returns false if the queue was full and the request dropped.
bool lobby_signal_join(uint64_t a_u64_nonce);

// Called by the DDS worker. Copies the oldest waiting answer to *a_p_out and
// returns true, or returns false if there is none.
bool lobby_take_result(JoinResult *a_p_out);

// Free the slot of a player that sent no Input for this many seconds
// (0 = never). Set by the server's --player-timeout option before starting.
extern int int_player_timeout_s;

// Called by the DDS worker when a Leave arrives. Returns false if the queue
// was full and the Leave dropped.
bool lobby_signal_leave(int a_int_player_id);

// Called repeatedly by the main thread: handles every pending leave, then
// every pending join request
void lobby_poll(void);

// Called by the main thread once per tick: frees the slots of players not
// heard from for int_player_timeout_s seconds (a_u64_now_ns: timing_now_ns())
void lobby_check_timeouts(uint64_t a_u64_now_ns);

// Startup only: reserve 0..MAX_PLAYERS-1 slots for server-controlled bots.
bool lobby_add_bots(int a_int_count);

#endif /* SERVER_LOBBY_H */
