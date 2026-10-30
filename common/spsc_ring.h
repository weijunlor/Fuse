/* common/spsc_ring.h
Lock-free single-producer/single-consumer ring buffer (C11 atomics). */

#ifndef COMMON_SPSC_RING_H
#define COMMON_SPSC_RING_H

#include <stdbool.h>
#include <stdatomic.h>
#include "cache_padding.h"

/* Exactly one producer calls push/space, and one consumer calls pop. Values
are copied into the ring; never enqueue pointers into DDS loaned storage.
Unsigned counters wrap naturally. A power-of-two capacity makes indexing work
across that wrap, and every slot is usable (no sentinel slot).

The head release publishes a complete value. The tail release tells the
producer when it may reuse that slot. Neither side waits: full/empty returns
false, leaving both the queue and the caller's output unchanged.

With PADDING=1, head, tail and the payload start on separate 64-byte lines.
PADDING=0 keeps the original compact layout; algorithms and memory orders are
identical. Head/tail are still read by both threads (true sharing remains). */
#define SPSC_RING_DEFINE(name, type, capacity)                                  \
    _Static_assert((capacity) > 0 &&                                           \
                   ((capacity) & ((capacity) - 1)) == 0,                       \
                   "SPSC capacity must be a power of two");                   \
    typedef struct {                                                          \
        FUSE_CACHE_ALIGN atomic_uint u_head;                                  \
        FUSE_CACHE_ALIGN atomic_uint u_tail;                                  \
        FUSE_CACHE_ALIGN type arr_values[capacity];                           \
    } name;                                                                   \
    static inline void name##_init(name *a_ring)                               \
    {                                                                         \
        atomic_init(&a_ring->u_head, 0);                                       \
        atomic_init(&a_ring->u_tail, 0);                                       \
    }                                                                         \
    static inline unsigned name##_space(name *a_ring)                          \
    {                                                                         \
        unsigned u_head = atomic_load_explicit(&a_ring->u_head, memory_order_relaxed); \
        unsigned u_tail = atomic_load_explicit(&a_ring->u_tail, memory_order_acquire); \
        return (capacity) - (unsigned)(u_head - u_tail);                       \
    }                                                                         \
    static inline bool name##_push(name *a_ring, const type *a_value)           \
    {                                                                         \
        unsigned u_head = atomic_load_explicit(&a_ring->u_head, memory_order_relaxed); \
        unsigned u_tail = atomic_load_explicit(&a_ring->u_tail, memory_order_acquire); \
        if ((unsigned)(u_head - u_tail) == (capacity))                          \
            return false;                                                     \
        a_ring->arr_values[u_head & ((capacity) - 1)] = *a_value;               \
        atomic_store_explicit(&a_ring->u_head, u_head + 1, memory_order_release); \
        return true;                                                          \
    }                                                                         \
    static inline bool name##_pop(name *a_ring, type *a_out)                    \
    {                                                                         \
        unsigned u_tail = atomic_load_explicit(&a_ring->u_tail, memory_order_relaxed); \
        unsigned u_head = atomic_load_explicit(&a_ring->u_head, memory_order_acquire); \
        if (u_tail == u_head)                                                  \
            return false;                                                     \
        *a_out = a_ring->arr_values[u_tail & ((capacity) - 1)];                 \
        atomic_store_explicit(&a_ring->u_tail, u_tail + 1, memory_order_release); \
        return true;                                                          \
    }

#endif /* COMMON_SPSC_RING_H */
