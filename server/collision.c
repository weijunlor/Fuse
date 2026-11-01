/* server/collision.c
AABB tests and spatial grid for embers.

Embers are stored in one of two layouts (b_ember_ring, #8b):
- array: arr_embers[MAX_EMBERS] of Ember structs with an active flag. Every
  tick, spawn and grid rebuild walks all 4096 slots (48 KB, a whole P-core
  L1 data cache) although an explosion makes only 32 embers.
- ring: every ember lives exactly EMBER_LIFETIME_TICKS, so embers expire in
  the order they were spawned. The live embers are therefore always one
  contiguous run of a FIFO ring: spawn appends at the tail, expiry pops the
  head. Each field has its own array (SoA): the tick reads only spawn ticks,
  the grid rebuild and queries read only 2-byte coordinates, and the query
  loops have no early exit inside a run, so GCC vectorises them. */

#include "collision.h"
#include "config.h"

#define EMBER_CELL_SHIFT 6
#define EMBER_CELL_SIZE (1 << EMBER_CELL_SHIFT)
#define EMBER_GRID_WIDTH ((WIDTH + EMBER_CELL_SIZE - 1) / EMBER_CELL_SIZE)
#define EMBER_GRID_HEIGHT ((HEIGHT + EMBER_CELL_SIZE - 1) / EMBER_CELL_SIZE)
#define EMBER_GRID_CELLS (EMBER_GRID_WIDTH * EMBER_GRID_HEIGHT)
#define EMBERS_PER_SPAWN 32
#define EMBER_SIZE 6
// Brute-force ring queries test this many embers between early-exit checks
#define EMBER_QUERY_BLOCK 64

bool b_collision_branchless = true;
bool b_ember_ring = true;

// Set by spawn/expiry; the next query rebuilds the grid of the active layout
static bool b_grid_dirty = false;
// Counting-sort active embers into contiguous cell ranges (#8). Queries
// read coordinates sequentially instead of following per-ember next indices.
// Shared by both layouts (only one is in use).
static int arr_int_cell_offsets[EMBER_GRID_CELLS + 1];

/* ---- array layout (before #8b) ---- */

static Ember arr_embers[MAX_EMBERS];
static Ember arr_sorted_embers[MAX_EMBERS];

/* ---- ring layout (#8b) ---- */

#define EMBER_RING_MASK (MAX_EMBERS - 1)
_Static_assert((MAX_EMBERS & EMBER_RING_MASK) == 0,
               "MAX_EMBERS must be a power of two (ring index mask)");
// Ember positions are explosion centres (0..WIDTH/HEIGHT) +-60, so they fit
// in 16 bits: 8 coordinates per 16-byte SSE register instead of 4.
static int16_t arr_i16_ring_x[MAX_EMBERS];
static int16_t arr_i16_ring_y[MAX_EMBERS];
static uint32_t arr_u32_ring_spawn[MAX_EMBERS];  // u32_ember_tick at spawn
static int int_ring_head = 0;   // index of the oldest live ember
static int int_ring_count = 0;  // live embers: head .. head + count - 1
// Calls of collision_embers_tick(); ages are differences, so wrap-around
// of the 32-bit counter is harmless.
static uint32_t u32_ember_tick = 0;
// The ring's grid: cell-sorted coordinates, also SoA
static int16_t arr_i16_sorted_x[MAX_EMBERS];
static int16_t arr_i16_sorted_y[MAX_EMBERS];

static bool ember_overlaps_player(const Ember *a_p_ember, int a_int_x,
                                  int a_int_y)
{
	return collision_overlap(a_p_ember->x, a_p_ember->y, EMBER_SIZE,
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

// Grid cells a player square at (x, y) can touch, clamped to the grid
static void player_cell_range(int a_int_x, int a_int_y,
                              int *a_p_min_cx, int *a_p_min_cy,
                              int *a_p_max_cx, int *a_p_max_cy)
{
	int int_min_x = a_int_x - (EMBER_SIZE - 1);
	int int_min_y = a_int_y - (EMBER_SIZE - 1);
	*a_p_min_cx = (int_min_x > 0 ? int_min_x : 0) >> EMBER_CELL_SHIFT;
	*a_p_min_cy = (int_min_y > 0 ? int_min_y : 0) >> EMBER_CELL_SHIFT;
	*a_p_max_cx = (a_int_x + SQ_WIDTH) >> EMBER_CELL_SHIFT;
	*a_p_max_cy = (a_int_y + SQ_WIDTH) >> EMBER_CELL_SHIFT;
	if (*a_p_max_cx >= EMBER_GRID_WIDTH)
		*a_p_max_cx = EMBER_GRID_WIDTH - 1;
	if (*a_p_max_cy >= EMBER_GRID_HEIGHT)
		*a_p_max_cy = EMBER_GRID_HEIGHT - 1;
}

/* ================= array layout ================= */

static void rebuild_ember_grid_array(void)
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

static void embers_spawn_array(int a_int_x, int a_int_y, uint16_t a_u16_seed)
{
	uint32_t u32_state = a_u16_seed;
	for (int i = 0; i < EMBERS_PER_SPAWN; i++)
	{
		// Linear search from slot 0 for every ember
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

static void embers_tick_array(void)
{
	// All 4096 slots, every tick, even when none is active
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

static bool hits_ember_bruteforce_array(int a_int_x, int a_int_y)
{
	for (int i = 0; i < MAX_EMBERS; i++)
	{
		if (arr_embers[i].b_active &&
		    ember_overlaps_player(&arr_embers[i], a_int_x, a_int_y))
			return true;
	}
	return false;
}

// noinline: compiled on its own, as the whole of the original
// collision_player_hits_ember() was. Inlined into the layout dispatcher,
// GCC laid out the #9 branchless test with an extra data-dependent branch
// (2.4x the branch misses, 35% slower), which would flatter the ring.
__attribute__((noinline))
static bool hits_ember_grid_array(int a_int_x, int a_int_y)
{
	if (b_grid_dirty)
		rebuild_ember_grid_array();
	// Kept exactly as measured for #8 (the before for #8b)
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

/* ================= ring layout (#8b) ================= */

// The live run of the ring as at most two contiguous index ranges
// [start, end): it wraps past the last slot when head + count > MAX_EMBERS.
static int ring_segments(int a_arr_int_start[2], int a_arr_int_end[2])
{
	int int_end = int_ring_head + int_ring_count;
	a_arr_int_start[0] = int_ring_head;
	a_arr_int_end[0] = int_end < MAX_EMBERS ? int_end : MAX_EMBERS;
	if (int_end <= MAX_EMBERS)
		return 1;
	a_arr_int_start[1] = 0;
	a_arr_int_end[1] = int_end - MAX_EMBERS;
	return 2;
}

/* Does any of n embers (coordinate arrays x, y) overlap the player square
 * at (px, py)? Same test as ember_overlaps_player() (strict edges), written
 * as an ember-inside-a-box check: the ember's corner must lie in
 * [px - 5, px + SQ_WIDTH - 1] x [py - 5, py + SQ_WIDTH - 1].
 * The four comparisons are combined with & and OR-ed into one flag with no
 * early exit, so the loop has a single exit and GCC vectorises it: 8 embers
 * per 16-byte SSE2 compare with 16-bit coordinates (16 with AVX2). Callers
 * keep runs short (one grid row, or EMBER_QUERY_BLOCK embers). */
static bool any_ember_overlaps(const int16_t *restrict a_p_x,
                               const int16_t *restrict a_p_y, int a_int_n,
                               int a_int_px, int a_int_py)
{
	// Player squares are inside the window, so the bounds fit in 16 bits
	// and the compares stay 16-bit (no widening in the vector loop)
	int16_t i16_min_x = (int16_t)(a_int_px - (EMBER_SIZE - 1));
	int16_t i16_max_x = (int16_t)(a_int_px + SQ_WIDTH - 1);
	int16_t i16_min_y = (int16_t)(a_int_py - (EMBER_SIZE - 1));
	int16_t i16_max_y = (int16_t)(a_int_py + SQ_WIDTH - 1);
	int16_t i16_hit = 0;
	for (int i = 0; i < a_int_n; i++)
		i16_hit |= (int16_t)((a_p_x[i] >= i16_min_x) & (a_p_x[i] <= i16_max_x) &
		                     (a_p_y[i] >= i16_min_y) & (a_p_y[i] <= i16_max_y));
	return i16_hit != 0;
}

static void rebuild_ember_grid_ring(void)
{
	int arr_int_start[2], arr_int_end[2];
	int int_segments = ring_segments(arr_int_start, arr_int_end);
	int arr_int_counts[EMBER_GRID_CELLS] = {0};
	// Only the live embers, reading only the coordinate arrays
	for (int s = 0; s < int_segments; s++)
		for (int i = arr_int_start[s]; i < arr_int_end[s]; i++)
			arr_int_counts[ember_cell(arr_i16_ring_x[i], arr_i16_ring_y[i])]++;
	arr_int_cell_offsets[0] = 0;
	for (int i = 0; i < EMBER_GRID_CELLS; i++)
		arr_int_cell_offsets[i + 1] = arr_int_cell_offsets[i] + arr_int_counts[i];
	int arr_int_cursor[EMBER_GRID_CELLS];
	for (int i = 0; i < EMBER_GRID_CELLS; i++)
		arr_int_cursor[i] = arr_int_cell_offsets[i];
	for (int s = 0; s < int_segments; s++)
	{
		for (int i = arr_int_start[s]; i < arr_int_end[s]; i++)
		{
			int int_slot = arr_int_cursor[ember_cell(arr_i16_ring_x[i], arr_i16_ring_y[i])]++;
			arr_i16_sorted_x[int_slot] = arr_i16_ring_x[i];
			arr_i16_sorted_y[int_slot] = arr_i16_ring_y[i];
		}
	}
	b_grid_dirty = false;
}

static void embers_spawn_ring(int a_int_x, int a_int_y, uint16_t a_u16_seed)
{
	// Same random offsets and the same "drop when full" rule as the array
	// layout, so both layouts hold the same set of embers
	uint32_t u32_state = a_u16_seed;
	for (int i = 0; i < EMBERS_PER_SPAWN && int_ring_count < MAX_EMBERS; i++)
	{
		u32_state = u32_state * 1664525u + 1013904223u;
		int int_dx = (int)(u32_state % 121u) - 60;
		u32_state = u32_state * 1664525u + 1013904223u;
		int int_dy = (int)(u32_state % 121u) - 60;
		// Append at the tail: no free-slot search
		int int_slot = (int_ring_head + int_ring_count) & EMBER_RING_MASK;
		arr_i16_ring_x[int_slot] = (int16_t)(a_int_x + int_dx);
		arr_i16_ring_y[int_slot] = (int16_t)(a_int_y + int_dy);
		arr_u32_ring_spawn[int_slot] = u32_ember_tick;
		int_ring_count++;
	}
	b_grid_dirty = true;
}

static void embers_tick_ring(void)
{
	// The oldest ember is at the head: pop while it has lived its lifetime.
	// Usually one comparison (or none when the ring is empty). An ember
	// expires on the same tick as in the array layout (age reaches the
	// lifetime after EMBER_LIFETIME_TICKS calls).
	while (int_ring_count > 0 &&
	       u32_ember_tick - arr_u32_ring_spawn[int_ring_head] >= EMBER_LIFETIME_TICKS)
	{
		int_ring_head = (int_ring_head + 1) & EMBER_RING_MASK;
		int_ring_count--;
		b_grid_dirty = true;
	}
}

static bool hits_ember_bruteforce_ring(int a_int_x, int a_int_y)
{
	// Every live ember (no grid), in blocks with an early exit between them
	int arr_int_start[2], arr_int_end[2];
	int int_segments = ring_segments(arr_int_start, arr_int_end);
	for (int s = 0; s < int_segments; s++)
	{
		for (int i = arr_int_start[s]; i < arr_int_end[s]; i += EMBER_QUERY_BLOCK)
		{
			int int_n = arr_int_end[s] - i;
			if (int_n > EMBER_QUERY_BLOCK)
				int_n = EMBER_QUERY_BLOCK;
			if (any_ember_overlaps(arr_i16_ring_x + i, arr_i16_ring_y + i, int_n,
			                       a_int_x, a_int_y))
				return true;
		}
	}
	return false;
}

static bool hits_ember_grid_ring(int a_int_x, int a_int_y)
{
	if (b_grid_dirty)
		rebuild_ember_grid_ring();
	int int_min_cell_x, int_min_cell_y, int_max_cell_x, int_max_cell_y;
	player_cell_range(a_int_x, a_int_y, &int_min_cell_x, &int_min_cell_y,
	                  &int_max_cell_x, &int_max_cell_y);
	// Cells are numbered row by row, so the cells of one grid row that the
	// player touches are one contiguous run of the sorted arrays: one
	// vectorised loop per row instead of one scalar loop per cell
	for (int y = int_min_cell_y; y <= int_max_cell_y; y++)
	{
		int int_first = arr_int_cell_offsets[y * EMBER_GRID_WIDTH + int_min_cell_x];
		int int_last = arr_int_cell_offsets[y * EMBER_GRID_WIDTH + int_max_cell_x + 1];
		if (any_ember_overlaps(arr_i16_sorted_x + int_first,
		                       arr_i16_sorted_y + int_first,
		                       int_last - int_first, a_int_x, a_int_y))
			return true;
	}
	return false;
}

/* ================= public functions ================= */

void collision_embers_clear(void)
{
	// Reset both layouts so a switch between runs starts clean
	for (int i = 0; i < MAX_EMBERS; i++)
		arr_embers[i].b_active = false;
	int_ring_head = int_ring_count = 0;
	for (int i = 0; i <= EMBER_GRID_CELLS; i++)
		arr_int_cell_offsets[i] = 0;
	b_grid_dirty = false;
}

void collision_embers_spawn(int a_int_x, int a_int_y, uint16_t a_u16_seed)
{
	if (b_ember_ring)
		embers_spawn_ring(a_int_x, a_int_y, a_u16_seed);
	else
		embers_spawn_array(a_int_x, a_int_y, a_u16_seed);
}

void collision_embers_tick(void)
{
	u32_ember_tick++;
	if (b_ember_ring)
		embers_tick_ring();
	else
		embers_tick_array();
}

bool collision_player_hits_ember_bruteforce(int a_int_x, int a_int_y)
{
	return b_ember_ring ? hits_ember_bruteforce_ring(a_int_x, a_int_y)
	                    : hits_ember_bruteforce_array(a_int_x, a_int_y);
}

bool collision_player_hits_ember(int a_int_x, int a_int_y)
{
	return b_ember_ring ? hits_ember_grid_ring(a_int_x, a_int_y)
	                    : hits_ember_grid_array(a_int_x, a_int_y);
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
