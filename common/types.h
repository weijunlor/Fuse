/** @file common/types.h
 * @brief Shared game types and the constants for the fields of idl/fuse.idl
 * (key bits, player flags, phases, join status, pickup types).
 */

#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>

/** @brief One player slot in the server's table (baseline layout).
 * @details The coordinate is the NW (top-left) corner of the square.
 */
typedef struct
{
    bool b_active;  ///< Slot in use
    int x;          ///< Left edge, pixels
    int y;          ///< Top edge, pixels
} Player;

/* Constants for the fields of idl/fuse.idl. Bit flags are powers of two so
they combine with |. */

/** @name Input.keys bits: keys held right now
 * @{ */
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x04
#define KEY_RIGHT 0x08
#define KEY_DASH  0x10  ///< A dash starts when this bit goes from up to down
/** @} */

/** @name PlayerState.flags bits
 * @{ */
#define PF_ACTIVE  0x01  ///< Slot in use
#define PF_ALIVE   0x02  ///< Has lives left
#define PF_SHIELD  0x04  ///< Next tag is blocked
#define PF_DASHING 0x08  ///< Dash in progress
#define PF_SPEED   0x10  ///< Speed pickup active
#define PF_BOT     0x20  ///< Server-controlled slot
/** @} */

/** @name GameSnapshot.phase values
 * @{ */
#define PHASE_LOBBY      0
#define PHASE_COUNTDOWN  1
#define PHASE_PLAYING    2
#define PHASE_ROUND_OVER 3
#define PHASE_GAME_OVER  4
/** @} */

/** @name JoinResponse.status values
 * @{ */
#define JOIN_OK        0
#define JOIN_FULL      1
#define JOIN_BAD_LOGIN 2
#define JOIN_IN_GAME   3  ///< Joining is only allowed in the lobby
#define JOIN_RETRY     4  ///< Temporary join rate limit; retry the same request
/** @} */

/** @name Pickup.type values
 * @{ */
#define PICKUP_NONE       0  ///< Empty slot
#define PICKUP_SPEED      1
#define PICKUP_SHIELD     2
#define PICKUP_FUSE_RESET 3  ///< Adds 3 seconds to the remaining fuse
/** @} */

#endif /* COMMON_TYPES_H */
