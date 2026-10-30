/* server/collision.c
AABB tests and spatial grid for embers. */

#include "collision.h"
#include "config.h"

#define EMBER_LIFETIME_TICKS (8 * TICK_HZ)
#define EMBER_CELL_SHIFT 6
#define EMBER_CELL_SIZE (1 << EMBER_CELL_SHIFT)
#define EMBER_GRID_WIDTH ((WIDTH + EMBER_CELL_SIZE - 1) / EMBER_CELL_SIZE)
#define EMBER_GRID_HEIGHT ((HEIGHT + EMBER_CELL_SIZE - 1) / EMBER_CELL_SIZE)
#define EMBER_GRID_CELLS (EMBER_GRID_WIDTH * EMBER_GRID_HEIGHT)

static Ember arr_embers[MAX_EMBERS];
static int arr_int_cell_heads[EMBER_GRID_CELLS];
static int arr_int_next[MAX_EMBERS];

static bool ember_overlaps_player(const Ember *a_p_ember, int a_int_x,
                                  int a_int_y)
{
	return a_p_ember->x < a_int_x + SQ_WIDTH &&
	       a_p_ember->x + 6 > a_int_x &&
	       a_p_ember->y < a_int_y + SQ_WIDTH &&
	       a_p_ember->y + 6 > a_int_y;
}

static int ember_cell(int a_int_x, int a_int_y)
{
	int int_cell_x = a_int_x >> EMBER_CELL_SHIFT;
	int int_cell_y = a_int_y >> EMBER_CELL_SHIFT;
	if (int_cell_x < 0)
		int_cell_x = 0;
	if (int_cell_x >= EMBER_GRID_WIDTH)
		int_cell_x = EMBER_GRID_WIDTH - 1;
	if (int_cell_y < 0)
		int_cell_y = 0;
	if (int_cell_y >= EMBER_GRID_HEIGHT)
		int_cell_y = EMBER_GRID_HEIGHT - 1;
	return int_cell_y * EMBER_GRID_WIDTH + int_cell_x;
}

static void rebuild_ember_grid(void)
{
	for (int i = 0; i < EMBER_GRID_CELLS; i++)
		arr_int_cell_heads[i] = -1;
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		arr_int_next[i] = -1;
		if (!arr_embers[i].b_active)
			continue;
		int int_cell = ember_cell(arr_embers[i].x, arr_embers[i].y);
		arr_int_next[i] = arr_int_cell_heads[int_cell];
		arr_int_cell_heads[int_cell] = i;
	}
}

void collision_embers_clear(void)
{
	for (int i = 0; i < MAX_EMBERS; i++)
		arr_embers[i].b_active = false;
	rebuild_ember_grid();
}

void collision_embers_spawn(int a_int_x, int a_int_y, uint16_t a_u16_seed)
{
	uint32_t u32_state = a_u16_seed;
	for (int i = 0; i < 32; i++)
	{
		int int_slot = -1;
		for (int j = 0; j < MAX_EMBERS; j++)
		{
			if (!arr_embers[j].b_active)
			{
				int_slot = j;
				break;
			}
		}
		if (int_slot < 0)
			break;
		u32_state = u32_state * 1664525u + 1013904223u;
		int int_dx = (int)(u32_state % 121u) - 60;
		u32_state = u32_state * 1664525u + 1013904223u;
		int int_dy = (int)(u32_state % 121u) - 60;
		arr_embers[int_slot] = (Ember){
			.x = a_int_x + int_dx,
			.y = a_int_y + int_dy,
			.u16_age_ticks = 0,
			.b_active = true
		};
	}
	rebuild_ember_grid();
}

void collision_embers_tick(void)
{
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		if (arr_embers[i].b_active &&
		    ++arr_embers[i].u16_age_ticks >= EMBER_LIFETIME_TICKS)
			arr_embers[i].b_active = false;
	}
	rebuild_ember_grid();
}

bool collision_player_hits_ember_bruteforce(int a_int_x, int a_int_y)
{
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		if (arr_embers[i].b_active &&
		    ember_overlaps_player(&arr_embers[i], a_int_x, a_int_y))
			return true;
	}
	return false;
}

bool collision_player_hits_ember(int a_int_x, int a_int_y)
{
	int int_min_x = a_int_x - 5;
	int int_min_y = a_int_y - 5;
	int int_min_cell_x = (int_min_x > 0 ? int_min_x : 0) >> EMBER_CELL_SHIFT;
	int int_min_cell_y = (int_min_y > 0 ? int_min_y : 0) >> EMBER_CELL_SHIFT;
	int int_max_cell_x = (a_int_x + SQ_WIDTH) >> EMBER_CELL_SHIFT;
	int int_max_cell_y = (a_int_y + SQ_WIDTH) >> EMBER_CELL_SHIFT;
	if (int_max_cell_x >= EMBER_GRID_WIDTH)
		int_max_cell_x = EMBER_GRID_WIDTH - 1;
	if (int_max_cell_y >= EMBER_GRID_HEIGHT)
		int_max_cell_y = EMBER_GRID_HEIGHT - 1;

	for (int y = int_min_cell_y; y <= int_max_cell_y; y++)
	{
		for (int x = int_min_cell_x; x <= int_max_cell_x; x++)
		{
			for (int i = arr_int_cell_heads[y * EMBER_GRID_WIDTH + x];
			     i >= 0; i = arr_int_next[i])
			{
				if (ember_overlaps_player(&arr_embers[i], a_int_x, a_int_y))
					return true;
			}
		}
	}
	return false;
}

bool collision_detect(Player a_arr_players[], int a_int_arr_len,
                      int a_int_id, int a_int_x, int a_int_y)
{
	for(int i = 0; i < a_int_arr_len; i++)
	{
		if (i != a_int_id - 1 && a_arr_players[i].b_active &&
		    a_int_x <= a_arr_players[i].x + SQ_WIDTH &&
		    a_arr_players[i].x <= a_int_x + SQ_WIDTH &&
		    a_int_y <= a_arr_players[i].y + SQ_WIDTH &&
		    a_arr_players[i].y <= a_int_y + SQ_WIDTH)
			return true;
	}
	return false;
}
