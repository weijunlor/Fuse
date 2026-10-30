/* client/interp.c
Snapshot buffer and interpolation between server ticks (optimisation #15).
How it works and why: see interp.h. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "interp.h"
#include "types.h"

bool b_interp = true;
bool b_interp_stats = false;

/* Ring of the last INTERP_SLOTS snapshots, oldest at int_first. Ticks rise
from oldest to newest (interp_push drops anything else). */
static GameSnapshot arr_snapshots[INTERP_SLOTS];
static int int_first = 0;
static int int_count = 0;

// Client time (seconds) minus server time (tick / TICK_HZ): when a tick
// "happens" on our clock if the snapshot arrived as fast as the fastest one
static double d_offset_s = 0.0;

// Smallest tick gap between consecutive snapshots seen: the server's publish
// interval (TICK_HZ / --snapshot-hz). A lost snapshot makes one gap larger,
// never smaller, so the minimum is the true interval.
static uint32_t u_interval_ticks = 0;

// Snapshot number a_int_i, counted from the oldest
static const GameSnapshot *slot(int a_int_i)
{
    return &arr_snapshots[(int_first + a_int_i) % INTERP_SLOTS];
}

// *a_p_out = a_p_snapshot as received (positions not interpolated)
static void frame_from(InterpFrame *a_p_out, const GameSnapshot *a_p_snapshot)
{
    a_p_out->snapshot = *a_p_snapshot;
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        a_p_out->arr_f_x[i] = a_p_snapshot->players[i].x;
        a_p_out->arr_f_y[i] = a_p_snapshot->players[i].y;
    }
}

void interp_push(const GameSnapshot *a_p_snapshot, uint64_t a_u_received_ns)
{
    double d_received_s = a_u_received_ns / 1e9;
    double d_estimate_s = d_received_s - (double)a_p_snapshot->tick / TICK_HZ;

    if (int_count > 0)
    {
        uint32_t u_newest = slot(int_count - 1)->tick;
        // Older than what we have (out of order) or the same tick: ignore
        if (a_p_snapshot->tick <= u_newest && a_p_snapshot->tick + TICK_HZ > u_newest)
            return;
        // Much lower tick: a new server started. Start again.
        if (a_p_snapshot->tick <= u_newest)
        {
            int_count = 0;
            u_interval_ticks = 0;
        }
        else
        {
            uint32_t u_gap = a_p_snapshot->tick - u_newest;
            if (u_interval_ticks == 0 || u_gap < u_interval_ticks)
                u_interval_ticks = u_gap;
        }
    }

    /* Clock offset: the fastest delivery seen so far. A later, slower
    delivery raises the estimate only slowly (1% per snapshot), so jitter does
    not move the render time, but a lasting change in delay (or clock drift)
    is followed within a few seconds. */
    if (int_count == 0 || d_estimate_s < d_offset_s)
        d_offset_s = d_estimate_s;
    else
        d_offset_s += (d_estimate_s - d_offset_s) * 0.01;

    // Append; when full, overwrite the oldest
    if (int_count == INTERP_SLOTS)
    {
        int_first = (int_first + 1) % INTERP_SLOTS;
        int_count--;
    }
    arr_snapshots[(int_first + int_count) % INTERP_SLOTS] = *a_p_snapshot;
    int_count++;
}

bool interp_sample(uint64_t a_u_now_ns, InterpFrame *a_p_out)
{
    if (int_count == 0)
    {
        memset(a_p_out, 0, sizeof *a_p_out);
        return false;
    }
    const GameSnapshot *p_newest = slot(int_count - 1);
    if (!b_interp)
    {
        frame_from(a_p_out, p_newest);
        return true;
    }

    // The server tick to draw: now on the server's clock, minus the delay
    double d_delay_ticks = (u_interval_ticks ? u_interval_ticks : 1) + INTERP_MARGIN_TICKS;
    double d_render_tick = (a_u_now_ns / 1e9 - d_offset_s) * TICK_HZ - d_delay_ticks;

    // Newest snapshot at or before the render tick (a), and the one after it (b)
    int int_a = -1;
    for (int i = int_count - 1; i >= 0; i--)
    {
        if ((double)slot(i)->tick <= d_render_tick)
        {
            int_a = i;
            break;
        }
    }
    if (int_a < 0)
    {
        // Render tick before everything we have (just started): oldest as is
        frame_from(a_p_out, slot(0));
        return true;
    }

    const GameSnapshot *p_a = slot(int_a);
    frame_from(a_p_out, p_a);   // everything but the positions comes from a

    if (int_a == int_count - 1)
    {
        // Nothing newer yet (a snapshot lost or late): carry on in the same
        // direction for a moment
        double d_ahead_s = (d_render_tick - p_a->tick) / TICK_HZ;
        if (d_ahead_s > INTERP_MAX_EXTRAP_S)
            d_ahead_s = INTERP_MAX_EXTRAP_S;
        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            a_p_out->arr_f_x[i] = (float)(p_a->players[i].x + p_a->players[i].vx * d_ahead_s);
            a_p_out->arr_f_y[i] = (float)(p_a->players[i].y + p_a->players[i].vy * d_ahead_s);
        }
        return true;
    }

    // Blend a and b by how far the render tick is between them
    const GameSnapshot *p_b = slot(int_a + 1);
    double d_t = (d_render_tick - p_a->tick) / (double)(p_b->tick - p_a->tick);
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        const PlayerState *p_pa = &p_a->players[i];
        const PlayerState *p_pb = &p_b->players[i];
        // Only a player present in both (one who just joined or left is
        // drawn where a says)
        if (!(p_pa->flags & PF_ACTIVE) || !(p_pb->flags & PF_ACTIVE))
            continue;
        a_p_out->arr_f_x[i] = (float)(p_pa->x + (p_pb->x - p_pa->x) * d_t);
        a_p_out->arr_f_y[i] = (float)(p_pa->y + (p_pb->y - p_pa->y) * d_t);
    }
    return true;
}

/* ----------------------------------------------------- */
/* Smoothness statistics (--smooth-stats)                */
/* ----------------------------------------------------- */

static int int_stat_frames = 0;
static uint64_t u_stat_first_ns = 0, u_stat_prev_ns = 0;
static InterpFrame stat_prev;
static long l_stat_samples = 0, l_stat_stalls = 0;
static double d_stat_sq_err = 0.0, d_stat_max_step = 0.0;

void interp_stats_frame(const InterpFrame *a_p_drawn, uint64_t a_u_now_ns)
{
    if (int_stat_frames++ == 0)
        u_stat_first_ns = a_u_now_ns;
    else
    {
        double d_dt_s = (a_u_now_ns - u_stat_prev_ns) / 1e9;
        for (int i = 0; i < MAX_PLAYERS && d_dt_s > 0; i++)
        {
            const PlayerState *p_now = &a_p_drawn->snapshot.players[i];
            const PlayerState *p_prev = &stat_prev.snapshot.players[i];
            // Real speed from the server's velocity, in this frame and the
            // last (so starts, stops and turns are left out)
            double d_speed = hypot(p_now->vx, p_now->vy);
            double d_prev_speed = hypot(p_prev->vx, p_prev->vy);
            if (!(p_now->flags & PF_ACTIVE) || !(p_prev->flags & PF_ACTIVE) ||
                d_speed == 0 || d_prev_speed == 0 ||
                p_now->vx != p_prev->vx || p_now->vy != p_prev->vy)
                continue;
            double d_step = hypot(a_p_drawn->arr_f_x[i] - stat_prev.arr_f_x[i],
                                  a_p_drawn->arr_f_y[i] - stat_prev.arr_f_y[i]);
            double d_err = d_step / d_dt_s - d_speed;
            l_stat_samples++;
            d_stat_sq_err += d_err * d_err;
            l_stat_stalls += d_step == 0;
            if (d_step > d_stat_max_step)
                d_stat_max_step = d_step;
        }
    }
    u_stat_prev_ns = a_u_now_ns;
    stat_prev = *a_p_drawn;
}

void interp_stats_print(void)
{
    double d_wall_s = (u_stat_prev_ns - u_stat_first_ns) / 1e9;
    printf("interp=%s interval_ticks=%u frames=%d fps=%.1f samples=%ld"
           " speed_rms_err=%.1f stall_pct=%.1f max_step_px=%.1f\n",
           b_interp ? "on" : "off", (unsigned)u_interval_ticks, int_stat_frames,
           d_wall_s > 0 ? (int_stat_frames - 1) / d_wall_s : 0.0, l_stat_samples,
           l_stat_samples ? sqrt(d_stat_sq_err / l_stat_samples) : 0.0,
           l_stat_samples ? 100.0 * l_stat_stalls / l_stat_samples : 0.0,
           d_stat_max_step);
    fflush(stdout);
}
