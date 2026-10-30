/* server/bot.h
AI bot state machine (stretch goal). */

#ifndef SERVER_BOT_H
#define SERVER_BOT_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "types.h"

// Slot ownership is changed by the lobby. Caller holds the game mutex (or
// runs during startup before the network thread exists).
void bot_set_player(int a_int_index, bool a_b_bot);
bool bot_is_player(int a_int_index);

// Deterministic chase/flee state machine. Reads server state; never sends DDS
// messages or bypasses the normal movement, collision and dash rules.
uint8_t bot_choose_keys(const Player a_arr_players[], const bool a_arr_alive[],
                        int a_int_index, uint8_t a_u8_holder, uint32_t a_u_tick);

#endif /* SERVER_BOT_H */
