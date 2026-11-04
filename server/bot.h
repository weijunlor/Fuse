/** @file server/bot.h
 * @brief Bot slot ownership and deterministic chase/flee controls.
 */

#ifndef SERVER_BOT_H
#define SERVER_BOT_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "types.h"

/** @brief Mark a slot as bot-controlled or human-controlled.
 * @param a_int_index Slot index in 0..MAX_PLAYERS-1; invalid indices are ignored.
 * @param a_b_bot True to reserve bot ownership, false to clear it.
 * @pre The caller holds the game mutex or runs before threads start.
 */
void bot_set_player(int a_int_index, bool a_b_bot);

/** @brief Check whether a slot is owned by a bot.
 * @param a_int_index Slot index to inspect.
 * @return True for a valid bot slot; false for human or invalid slots.
 * @pre Access is serialised by the game mutex or startup ordering.
 */
bool bot_is_player(int a_int_index);

/** @brief Choose movement and dash keys for one bot.
 * @details Chases the nearest living opponent when holding the bomb; otherwise
 * flees the holder and steers along walls. Uses normal game movement rules.
 * @param a_arr_players Current positions, with MAX_PLAYERS entries.
 * @param a_arr_alive Eligibility flags, with MAX_PLAYERS entries.
 * @param a_int_index Bot slot index in 0..MAX_PLAYERS-1.
 * @param a_u8_holder Current bomb-holder ID, or 0 for none.
 * @param a_u_tick Server tick, used to stagger dash presses.
 * @return KEY_* bitmask, or 0 when inactive or without a target.
 * @pre The caller holds the game mutex while reading these arrays.
 */
uint8_t bot_choose_keys(const Player a_arr_players[], const bool a_arr_alive[],
                        int a_int_index, uint8_t a_u8_holder, uint32_t a_u_tick);

#endif /* SERVER_BOT_H */
