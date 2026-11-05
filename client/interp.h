/** @file client/interp.h
 * @brief Snapshot buffer and interpolation between server ticks
 * (optimisation #15), with a between-session reset for the lobby.
 * @details interp_reset() prevents a later lobby session from drawing the
 * previous session's history.
 *
 * Without interpolation the client draws the newest snapshot. Positions then
 * change only when a snapshot arrives: at 60 snapshots/s on a 120-165 Hz
 * display some frames repeat the last position and others jump two steps, and
 * at 20 snapshots/s players visibly hop 9 px at a time. The server would have
 * to keep sending 60 snapshots/s to look smooth.
 *
 * With interpolation the client keeps the last INTERP_SLOTS snapshots and draws
 * the game a little in the past, at a "render tick" between two snapshots it
 * already has, blending the positions of the two by how far the render tick is
 * between them. Every frame then shows a new position, the movement is as
 * smooth at 20 snapshots/s as at 60, and the server can send 3x less.
 *
 * Time base: each snapshot carries the server tick it was made at (TICK_HZ per
 * second). The client estimates the offset between its clock and the server's
 * tick clock from the arrival times (the smallest "arrival time - tick time"
 * seen, i.e. the fastest delivery), so network jitter does not shake the
 * picture. render tick = server tick now - delay, where the delay is one
 * snapshot interval plus INTERP_MARGIN_TICKS (2 ticks = 33 ms at 60 Hz, 4 ticks
 * = 67 ms at 20 Hz, 7 = 117 ms at 10 Hz). If a snapshot is lost the client
 * extrapolates from the newest one by its velocity for at most
 * INTERP_MAX_EXTRAP_S.
 *
 * Threads: the render thread calls both interp_push() and interp_sample().
 * It receives copied snapshots and their arrival times through ClientFrame
 * (net_client.h); interpolation history is never shared with the DDS worker.
 */

#ifndef CLIENT_INTERP_H
#define CLIENT_INTERP_H

#include <stdbool.h>
#include <stdint.h>

#include "fuse.h"
#include "config.h"

/** @brief One frame's game state to draw.
 * @details The snapshot fields (phase, bomb, lives, pickups, ...) as
 * received, and every player's position as floats, which may lie between
 * whole pixels while interpolating (snapshot.players[i].x/y keep the received
 * whole-pixel values).
 */
typedef struct
{
    GameSnapshot snapshot;          ///< Snapshot at or before the render tick
    float arr_f_x[MAX_PLAYERS];     ///< Drawn x per player slot, pixels
    float arr_f_y[MAX_PLAYERS];     ///< Drawn y per player slot, pixels
} InterpFrame;

/** @brief Snapshots kept (at 60 Hz: 267 ms of history, at 10 Hz: 1.6 s). */
#define INTERP_SLOTS 16
/** @brief Extra delay on top of one snapshot interval, for network jitter (ticks). */
#define INTERP_MARGIN_TICKS 1.0
/** @brief Longest a position is extrapolated past the newest snapshot (seconds).
 * @details Short, because a player that stopped would overshoot and snap back. */
#define INTERP_MAX_EXTRAP_S 0.05

/** @brief True (default) = draw interpolated positions; false (--interp off) =
 * draw the newest snapshot as received (the "before" of #15). */
extern bool b_interp;

/** @brief True (--smooth-stats) = render_frame() calls interp_stats_frame()
 * every frame. */
extern bool b_interp_stats;

/** @brief Discard snapshot history and server-clock estimates between sessions.
 * @note Render thread only; interpolation options and measurement flags persist.
 */
void interp_reset(void);

/** @brief Store a received snapshot and update the server-clock estimate.
 * @details Snapshots older than the newest one are ignored; a tick much lower
 * than any seen before means a new server, and the buffer starts again.
 * @param a_p_snapshot Snapshot to copy into the ring.
 * @param a_u_received_ns Arrival time from timing_now_ns().
 * @note Render thread only.
 */
void interp_push(const GameSnapshot *a_p_snapshot, uint64_t a_u_received_ns);

/** @brief Game state to draw at a given time.
 * @details The snapshot at or just before the render tick, with every
 * player's position interpolated (or the newest snapshot if b_interp is false).
 * @param a_u_now_ns Current time from timing_now_ns().
 * @param[out] a_p_out State to draw; zeroed if no snapshot has arrived.
 * @return False if no snapshot has arrived yet.
 * @note Render thread only.
 */
bool interp_sample(uint64_t a_u_now_ns, InterpFrame *a_p_out);

/** @brief Smoothness measurement for one frame (--smooth-stats S in main.c).
 * @details Call once per frame with the state drawn; for every player moving
 * steadily (its snapshot velocity non-zero and unchanged in this frame and
 * the last) it compares the on-screen speed with the real speed.
 * @param a_p_drawn State drawn this frame.
 * @param a_u_now_ns Current time from timing_now_ns().
 */
void interp_stats_frame(const InterpFrame *a_p_drawn, uint64_t a_u_now_ns);

/** @brief Print the smoothness statistics as one key=value line.
 * @details Fields: frames, fps, samples (player-frames moving steadily),
 * speed_rms_err (px/s: RMS of on-screen speed minus real speed), stall_pct
 * (% of those player-frames where the player did not move on screen),
 * max_step_px (largest on-screen jump in one frame). Parsed by bench/smooth.sh.
 */
void interp_stats_print(void);

#endif /* CLIENT_INTERP_H */
