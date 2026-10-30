/* client/interp.h
Snapshot buffer and interpolation between server ticks (optimisation #15).

Without interpolation the client draws the newest snapshot. Positions then
change only when a snapshot arrives: at 60 snapshots/s on a 120-165 Hz
display some frames repeat the last position and others jump two steps, and
at 20 snapshots/s players visibly hop 9 px at a time. The server would have
to keep sending 60 snapshots/s to look smooth.

With interpolation the client keeps the last INTERP_SLOTS snapshots and draws
the game a little in the past, at a "render tick" between two snapshots it
already has, blending the positions of the two by how far the render tick is
between them. Every frame then shows a new position, the movement is as
smooth at 20 snapshots/s as at 60, and the server can send 3x less.

Time base: each snapshot carries the server tick it was made at (TICK_HZ per
second). The client estimates the offset between its clock and the server's
tick clock from the arrival times (the smallest "arrival time - tick time"
seen, i.e. the fastest delivery), so network jitter does not shake the
picture. render tick = server tick now - delay, where the delay is one
snapshot interval plus INTERP_MARGIN_TICKS (2 ticks = 33 ms at 60 Hz, 4 ticks
= 67 ms at 20 Hz, 7 = 117 ms at 10 Hz). If a snapshot is lost the client
extrapolates from the newest one by its velocity for at most
INTERP_MAX_EXTRAP_S.

Threads: the render thread calls both interp_push() and interp_sample().
It receives copied snapshots and their arrival times through ClientFrame
(net_client.h); interpolation history is never shared with the DDS worker. */

#ifndef CLIENT_INTERP_H
#define CLIENT_INTERP_H

#include <stdbool.h>
#include <stdint.h>

#include "fuse.h"
#include "config.h"

/* One frame's game state to draw: the snapshot fields (phase, bomb, lives,
pickups, ...) as received, and every player's position as floats, which may
lie between whole pixels while interpolating (snapshot.players[i].x/y keep
the received whole-pixel values). */
typedef struct
{
    GameSnapshot snapshot;
    float arr_f_x[MAX_PLAYERS];
    float arr_f_y[MAX_PLAYERS];
} InterpFrame;

// Snapshots kept (at 60 Hz: 267 ms of history, at 10 Hz: 1.6 s)
#define INTERP_SLOTS 16
// Extra delay on top of one snapshot interval, for network jitter (ticks)
#define INTERP_MARGIN_TICKS 1.0
// Longest a position is extrapolated past the newest snapshot (seconds);
// short, because a player that stopped would overshoot and snap back
#define INTERP_MAX_EXTRAP_S 0.05

// true (default) = draw interpolated positions; false (--interp off) = draw
// the newest snapshot as received (the "before" of #15)
extern bool b_interp;

// true (--smooth-stats) = render_frame() calls interp_stats_frame() every frame
extern bool b_interp_stats;

/* Store a snapshot received at a_u_received_ns (timing_now_ns()). Snapshots
older than the newest one are ignored; a tick lower than any seen before
means a new server, and the buffer starts again. Render thread only. */
void interp_push(const GameSnapshot *a_p_snapshot, uint64_t a_u_received_ns);

/* Fill *a_p_out with the game state to draw at a_u_now_ns: the snapshot at or
just before the render tick with every player's position interpolated (or the
newest snapshot if b_interp is false). Returns false if no snapshot has
arrived yet (*a_p_out is then zeroed). Render thread only. */
bool interp_sample(uint64_t a_u_now_ns, InterpFrame *a_p_out);

/* Smoothness measurement (--smooth-stats S in main.c). Call once per frame
with the state drawn; for every player moving steadily (its snapshot
velocity non-zero in this frame and the last) it compares the on-screen
speed with the real speed. */
void interp_stats_frame(const InterpFrame *a_p_drawn, uint64_t a_u_now_ns);

/* Print the smoothness statistics as one key=value line:
  frames, fps, samples (player-frames moving steadily),
  speed_rms_err (px/s: RMS of on-screen speed minus real speed),
  stall_pct (% of those player-frames where the player did not move on
  screen), max_step_px (largest on-screen jump in one frame). */
void interp_stats_print(void);

#endif /* CLIENT_INTERP_H */
