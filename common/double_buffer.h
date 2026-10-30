/* common/double_buffer.h
Nonblocking SPSC latest-value handoff with two buffers. */

#ifndef COMMON_DOUBLE_BUFFER_H
#define COMMON_DOUBLE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "cache_padding.h"

/* Bit 0 is the published slot; bits 1/2 pin slots 0/1 while the consumer
copies. A producer only writes the OTHER slot, and skips that publication if
it is pinned. The consumer pins the currently published slot with one CAS;
on a collision it keeps its previous frame and tries on its next pass.

Pinning matters: merely flipping an atomic index (or retrying a seqlock copy)
does not prevent a C data race when a fast writer laps a reader. Here a
published slot cannot be overwritten until its reader releases the pin.
Acquire/release orders both publication and reuse; every change to u_state
is an atomic RMW, preserving release sequences between the two threads.

There is one producer and one consumer, each using its own version counter.
Intermediate versions may be coalesced; neither thread spins or blocks.
Initialise before starting threads. Do not reset a live buffer.
PADDING=1 separates the shared state, the two thread-owned counters and the
two slots. The shared state still requires true cache-line sharing. */
#define DOUBLE_BUFFER_DEFINE(name, type)                                       \
    typedef struct {                                                          \
        FUSE_CACHE_ALIGN uint64_t u64_version;                                \
        type value;                                                           \
    } name##_Slot;                                                            \
    typedef struct {                                                          \
        FUSE_CACHE_ALIGN atomic_uint u_state;                                 \
        FUSE_CACHE_ALIGN uint64_t u64_written;                                \
        FUSE_CACHE_ALIGN uint64_t u64_seen;                                   \
        name##_Slot arr_slots[2];                                             \
    } name;                                                                   \
    static inline void name##_init(name *a_buffer)                             \
    {                                                                         \
        atomic_init(&a_buffer->u_state, 0);                                    \
        a_buffer->u64_written = a_buffer->u64_seen = 0;                        \
        a_buffer->arr_slots[0].u64_version = 0;                                \
        a_buffer->arr_slots[1].u64_version = 0;                                \
    }                                                                         \
    static inline bool name##_publish(name *a_buffer, const type *a_value)     \
    {                                                                         \
        unsigned u_state = atomic_load_explicit(&a_buffer->u_state, memory_order_acquire); \
        unsigned u_slot = (u_state & 1u) ^ 1u;                                \
        if (u_state & (2u << u_slot))                                         \
            return false;                                                     \
        a_buffer->arr_slots[u_slot].value = *a_value;                          \
        a_buffer->arr_slots[u_slot].u64_version = ++a_buffer->u64_written;     \
        atomic_fetch_xor_explicit(&a_buffer->u_state, 1u, memory_order_release); \
        return true;                                                          \
    }                                                                         \
    static inline bool name##_take(name *a_buffer, type *a_out)                \
    {                                                                         \
        unsigned u_state = atomic_load_explicit(&a_buffer->u_state, memory_order_relaxed); \
        unsigned u_slot = u_state & 1u, u_pin = 2u << u_slot;                  \
        if (!atomic_compare_exchange_strong_explicit(&a_buffer->u_state,       \
                &u_state, u_state | u_pin, memory_order_acquire, memory_order_relaxed)) \
            return false;                                                     \
        uint64_t u64_version = a_buffer->arr_slots[u_slot].u64_version;        \
        bool b_new = u64_version != a_buffer->u64_seen;                        \
        if (b_new)                                                            \
        {                                                                     \
            *a_out = a_buffer->arr_slots[u_slot].value;                        \
            a_buffer->u64_seen = u64_version;                                 \
        }                                                                     \
        atomic_fetch_and_explicit(&a_buffer->u_state, ~u_pin, memory_order_release); \
        return b_new;                                                         \
    }

#endif /* COMMON_DOUBLE_BUFFER_H */
