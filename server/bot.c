/* server/bot.c
AI bot state machine (stretch goal). */

#include "bot.h"

#include <limits.h>
#include <stdlib.h>

static bool arr_b_bot[MAX_PLAYERS];

void bot_set_player(int a_int_index, bool a_b_bot)
{
	if (a_int_index >= 0 && a_int_index < MAX_PLAYERS)
		arr_b_bot[a_int_index] = a_b_bot;
}

bool bot_is_player(int a_int_index)
{
	return a_int_index >= 0 && a_int_index < MAX_PLAYERS && arr_b_bot[a_int_index];
}

uint8_t bot_choose_keys(const Player a_arr_players[], const bool a_arr_alive[],
                        int a_int_index, uint8_t a_u8_holder, uint32_t a_u_tick)
{
	if (a_int_index < 0 || a_int_index >= MAX_PLAYERS || !a_arr_alive[a_int_index])
		return 0;
	bool b_chasing = a_u8_holder == a_int_index + 1;
	int int_target = -1;
	int int_distance = INT_MAX;
	for (int i = 0; i < MAX_PLAYERS; i++)
	{
		if (i == a_int_index || !a_arr_alive[i]) continue;
		if (!b_chasing && a_u8_holder != i + 1) continue;
		int int_dx = a_arr_players[i].x - a_arr_players[a_int_index].x;
		int int_dy = a_arr_players[i].y - a_arr_players[a_int_index].y;
		int int_squared = int_dx * int_dx + int_dy * int_dy;
		if (int_squared < int_distance)
		{
			int_target = i;
			int_distance = int_squared;
		}
	}
	if (int_target < 0) return 0;
	int int_dx = a_arr_players[int_target].x - a_arr_players[a_int_index].x;
	int int_dy = a_arr_players[int_target].y - a_arr_players[a_int_index].y;
	if (!b_chasing)
	{
		int_dx = -int_dx;
		int_dy = -int_dy;
		// Slide along walls instead of fleeing straight into a corner. A
		// deterministic side choice breaks ties when both axes line up.
		int int_x = a_arr_players[a_int_index].x;
		int int_y = a_arr_players[a_int_index].y;
		if ((int_x <= 6 && int_dx < 0) || (int_x >= WIDTH - SQ_WIDTH - 6 && int_dx > 0))
		{
			int_dx = 0;
			int_dy = int_y < (HEIGHT - SQ_WIDTH) / 2 ? 1 : -1;
		}
		if ((int_y <= 6 && int_dy < 0) || (int_y >= HEIGHT - SQ_WIDTH - 6 && int_dy > 0))
		{
			int_dy = 0;
			int_dx = int_x < (WIDTH - SQ_WIDTH) / 2 ? 1 : -1;
		}
	}
	uint8_t u8_keys = 0;
	if (int_dx < 0) u8_keys |= KEY_LEFT;
	if (int_dx > 0) u8_keys |= KEY_RIGHT;
	if (int_dy < 0) u8_keys |= KEY_UP;
	if (int_dy > 0) u8_keys |= KEY_DOWN;
	// One-tick press every 1.5 s allows the normal edge/cooldown logic to
	// re-arm. Stagger bots so they do not all dash on the same tick.
	if ((b_chasing || int_distance < 200 * 200) &&
	    (a_u_tick + (uint32_t)a_int_index * 17u) % (TICK_HZ * 3 / 2) == 0)
		u8_keys |= KEY_DASH;
	return u8_keys;
}
