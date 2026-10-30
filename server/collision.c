/* server/collision.c
AABB tests and spatial grid for embers. */

#include "collision.h"
#include "config.h"

#define EMBER_CELL_SHIFT 6
#define EMBER_CELL_SIZE (1 << EMBER_CELL_SHIFT)
#define EMBER_GRID_WIDTH ((WIDTH + EMBER_CELL_SIZE - 1) / EMBER_CELL_SIZE)
#define EMBER_GRID_HEIGHT ((HEIGHT + EMBER_CELL_SIZE - 1) / EMBER_CELL_SIZE)
#define EMBER_GRID_CELLS (EMBER_GRID_WIDTH * EMBER_GRID_HEIGHT)

static Ember arr_embers[MAX_EMBERS];
// Counting-sort active embers into contiguous cell ranges (#8). Queries
// read coordinates sequentially instead of following per-ember next indices.
static Ember arr_sorted_embers[MAX_EMBERS];
static int arr_int_cell_offsets[EMBER_GRID_CELLS + 1];
static bool b_grid_dirty = false;
bool b_collision_branchless = true;

static bool ember_overlaps_player(const Ember *a_p_ember, int a_int_x,
                                  int a_int_y)
{
	return collision_overlap(a_p_ember->x, a_p_ember->y, 6,
	                         a_int_x, a_int_y, SQ_WIDTH, false,
	                         b_collision_branchless);
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
	int arr_int_counts[EMBER_GRID_CELLS] = {0};
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		if (arr_embers[i].b_active)
			arr_int_counts[ember_cell(arr_embers[i].x, arr_embers[i].y)]++;
	}
	arr_int_cell_offsets[0] = 0;
	for (int i = 0; i < EMBER_GRID_CELLS; i++)
		arr_int_cell_offsets[i + 1] = arr_int_cell_offsets[i] + arr_int_counts[i];
	int arr_int_cursor[EMBER_GRID_CELLS];
	for (int i = 0; i < EMBER_GRID_CELLS; i++)
		arr_int_cursor[i] = arr_int_cell_offsets[i];
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		if (arr_embers[i].b_active)
		{
			int int_cell = ember_cell(arr_embers[i].x, arr_embers[i].y);
			arr_sorted_embers[arr_int_cursor[int_cell]++] = arr_embers[i];
		}
	}
	b_grid_dirty = false;
}

void collision_embers_clear(void)
{
	for (int i = 0; i < MAX_EMBERS; i++)
		arr_embers[i].b_active = false;
	for (int i = 0; i <= EMBER_GRID_CELLS; i++)
		arr_int_cell_offsets[i] = 0;
	b_grid_dirty = false;
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
	b_grid_dirty = true;
}

void collision_embers_tick(void)
{
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		if (arr_embers[i].b_active &&
		    ++arr_embers[i].u16_age_ticks >= EMBER_LIFETIME_TICKS)
		{
			arr_embers[i].b_active = false;
			b_grid_dirty = true;
		}
	}
	// Positions are fixed. Rebuild only on spawn/expiry, not every tick.
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
	if (b_grid_dirty)
		rebuild_ember_grid();
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
			int int_cell = y * EMBER_GRID_WIDTH + x;
			for (int i = arr_int_cell_offsets[int_cell];
			     i < arr_int_cell_offsets[int_cell + 1]; i++)
			{
				if (ember_overlaps_player(&arr_sorted_embers[i], a_int_x, a_int_y))
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
		    collision_overlap(a_int_x, a_int_y, SQ_WIDTH,
		                      a_arr_players[i].x, a_arr_players[i].y,
		                      SQ_WIDTH, true, b_collision_branchless))
			return true;
	}
	return false;
}
