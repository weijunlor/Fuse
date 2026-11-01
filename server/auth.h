/* server/auth.h
Login check, session tokens, input validation, rate limiting. */

#ifndef SERVER_AUTH_H
#define SERVER_AUTH_H

#include <stdbool.h>
#include <stdint.h>
#include "fuse.h"

#define AUTH_MAX_USERS 32
#define AUTH_MAX_JOINS 4096
#define AUTH_INPUT_RATE 120
#define AUTH_INPUT_BURST 30

// Load once in main, before creating threads. Never falls back to anonymous.
bool auth_init(void);
bool auth_enabled(void);
/* Everything below is called with game.h's mutex held. Sessions and player
slots therefore change together. a_free_id is 0 when the game is full.
Retries of an active login return the same session without resetting seq. */
void auth_join(const JoinRequest *a_request, int a_free_id, uint64_t a_now_ns,
               JoinResponse *a_response);
bool auth_input(const Input *a_input, uint64_t a_now_ns);
bool auth_leave(const Leave *a_leave);
void auth_forget(int a_player_id);

#endif /* SERVER_AUTH_H */
