/* common/config.h
Game constants shared by server and client: window size, MAX_PLAYERS, tick rate, tuning values. */

#ifndef COMMON_CONFIG_H
#define COMMON_CONFIG_H

// Maximum number of players in a game
#define MAX_PLAYERS 5

// Window (and play area) size in pixels
#define WIDTH  800
#define HEIGHT 600

// Side length of a player square in pixels
#define SQ_WIDTH 50

// Server simulation rate: the game advances in fixed steps of 1/TICK_HZ s,
// whatever the network or input rate (the baseline moved a player 1 pixel per
// input message, so speed depended on how fast messages arrived)
#define TICK_HZ 60

// Player speed in pixels per second while a direction key is held.
// A multiple of TICK_HZ, so positions stay whole pixels (3 px per tick).
#define PLAYER_SPEED 180

// Snapshots the server publishes per second unless --snapshot-hz says
// otherwise (one GameSnapshot every TICK_HZ / SNAPSHOT_HZ ticks)
#define SNAPSHOT_HZ 60

#endif /* COMMON_CONFIG_H */
