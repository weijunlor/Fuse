/** @file common/config.h
 * @brief Game constants shared by server and client: window size,
 * MAX_PLAYERS, tick rate and tuning values.
 */

#ifndef COMMON_CONFIG_H
#define COMMON_CONFIG_H

/** @brief Maximum number of players in a game (player IDs 1..MAX_PLAYERS). */
#define MAX_PLAYERS 5

/** @brief Window (and play area) width in pixels. */
#define WIDTH  800
/** @brief Window (and play area) height in pixels. */
#define HEIGHT 600

/** @brief Side length of a player square (hitbox) in pixels. */
#define SQ_WIDTH 50

/** @brief Server simulation rate, ticks per second (#12).
 * @details The game advances in fixed steps of 1/TICK_HZ s, whatever the
 * network or input rate (the baseline moved a player 1 pixel per input
 * message, so speed depended on how fast messages arrived). */
#define TICK_HZ 60

/** @brief Player speed in pixels per second while a direction key is held.
 * @details A multiple of TICK_HZ, so positions stay whole pixels (3 px per tick). */
#define PLAYER_SPEED 180

/** @brief Snapshots the server publishes per second unless --snapshot-hz says
 * otherwise (one GameSnapshot every TICK_HZ / SNAPSHOT_HZ ticks). */
#define SNAPSHOT_HZ 60

/** @brief Seconds without Input after which the server removes a player.
 * @details Unless the server's --player-timeout says otherwise (0 = never).
 * Clients repeat their keys every 100 ms even when idle (#13), so only a
 * crashed, killed or cut-off client is that quiet. Long enough that a short
 * network stall does not drop anyone (a clean exit sends Leave and frees the
 * slot at once). */
#define PLAYER_TIMEOUT_S 5

#endif /* COMMON_CONFIG_H */
