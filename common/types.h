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

#endif /* COMMON_TYPES_H */
