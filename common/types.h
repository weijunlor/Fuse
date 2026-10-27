/* common/types.h
Shared game types: PlayerState, snapshot layout, key bitmask flags. */

#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>

// Player coord defined as NW corner of square
typedef struct
{
    bool b_active;
    int x;
    int y;
} Player;

/* Constants for the fields of idl/fuse.idl (not used until the server
switches to snapshots). Bit flags are powers of two so they combine with |. */

// Input.keys: keys held right now
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x04
#define KEY_RIGHT 0x08
#define KEY_DASH  0x10

// PlayerState.flags
#define PF_ACTIVE  0x01  // slot in use
#define PF_ALIVE   0x02  // has lives left
#define PF_SHIELD  0x04  // next tag is blocked
#define PF_DASHING 0x08
#define PF_SPEED   0x10  // speed pickup active

// GameSnapshot.phase
#define PHASE_LOBBY      0
#define PHASE_COUNTDOWN  1
#define PHASE_PLAYING    2
#define PHASE_ROUND_OVER 3
#define PHASE_GAME_OVER  4

// JoinResponse.status
#define JOIN_OK        0
#define JOIN_FULL      1
#define JOIN_BAD_LOGIN 2
#define JOIN_IN_GAME   3  // joining is only allowed in the lobby

// Pickup.type
#define PICKUP_NONE       0  // empty slot
#define PICKUP_SPEED      1
#define PICKUP_SHIELD     2
#define PICKUP_FUSE_RESET 3

#endif /* COMMON_TYPES_H */
