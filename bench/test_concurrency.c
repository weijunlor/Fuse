/* Correctness stress tests for #3. No DDS, renderer or timing thresholds. */
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "spsc_ring.h"
#include "double_buffer.h"

#define CHECK(condition) do { if (!(condition)) {                              \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

#define TRANSFERS 250000u
typedef struct { uint64_t u64_seq; uint64_t arr_words[32]; } Value;
SPSC_RING_DEFINE(TestRing, Value, 8)
DOUBLE_BUFFER_DEFINE(TestBuffer, Value)
static TestRing ring;
static TestBuffer buffer;
static atomic_bool b_done;

static Value make_value(uint64_t a_seq)
{
    Value value = { .u64_seq = a_seq };
    for (unsigned i = 0; i < 32; i++)
        value.arr_words[i] = a_seq * 37 + i;
    return value;
}

static void check_value(const Value *a_value)
{
    for (unsigned i = 0; i < 32; i++)
        CHECK(a_value->arr_words[i] == a_value->u64_seq * 37 + i);
}

static void *ring_writer(void *a_unused)
{
    (void)a_unused;
    for (unsigned i = 1; i <= TRANSFERS; i++)
    {
        Value value = make_value(i);
        while (!TestRing_push(&ring, &value)) sched_yield();
    }
    return NULL;
}

static void *ring_reader(void *a_unused)
{
    (void)a_unused;
    for (unsigned i = 1; i <= TRANSFERS; i++)
    {
        Value value;
        while (!TestRing_pop(&ring, &value)) sched_yield();
        CHECK(value.u64_seq == i); // no lost, duplicated or reordered values
        check_value(&value);
        if (i % 997 == 0) sched_yield();
    }
    return NULL;
}

static void *buffer_writer(void *a_unused)
{
    (void)a_unused;
    for (unsigned i = 1; i <= TRANSFERS; i++)
    {
        Value value = make_value(i);
        // Intermediate snapshots may be skipped; the final one must arrive.
        if (i == TRANSFERS)
            while (!TestBuffer_publish(&buffer, &value)) sched_yield();
        else
            TestBuffer_publish(&buffer, &value);
    }
    atomic_store_explicit(&b_done, true, memory_order_release);
    return NULL;
}

static void *buffer_reader(void *a_unused)
{
    (void)a_unused;
    Value value = {0};
    uint64_t u64_last = 0;
    while (u64_last != TRANSFERS)
    {
        if (TestBuffer_take(&buffer, &value))
        {
            CHECK(value.u64_seq > u64_last);
            check_value(&value); // deliberately larger than a real snapshot
            u64_last = value.u64_seq;
            sched_yield(); // writer can lap this slower consumer
        }
        else if (atomic_load_explicit(&b_done, memory_order_acquire))
            sched_yield();
    }
    return NULL;
}

int main(void)
{
    TestRing_init(&ring);
    CHECK(atomic_is_lock_free(&ring.u_head) && atomic_is_lock_free(&ring.u_tail));
    Value value = make_value(999), out = value;
    CHECK(!TestRing_pop(&ring, &out) && out.u64_seq == 999);
    // Begin close to unsigned wrap and use every slot, then reject overflow.
    atomic_store(&ring.u_head, UINT_MAX - 3u);
    atomic_store(&ring.u_tail, UINT_MAX - 3u);
    for (unsigned i = 0; i < 8; i++)
    {
        value = make_value(i);
        CHECK(TestRing_push(&ring, &value));
    }
    CHECK(TestRing_space(&ring) == 0 && !TestRing_push(&ring, &value));
    for (unsigned i = 0; i < 8; i++)
    {
        CHECK(TestRing_pop(&ring, &out) && out.u64_seq == i);
        check_value(&out);
    }
    CHECK(TestRing_space(&ring) == 8 && !TestRing_pop(&ring, &out));
    puts("PASS ring empty/full/FIFO/counter wrap");

    TestBuffer_init(&buffer);
    CHECK(atomic_is_lock_free(&buffer.u_state));
    out = make_value(999);
    CHECK(!TestBuffer_take(&buffer, &out) && out.u64_seq == 999);
    value = make_value(1); CHECK(TestBuffer_publish(&buffer, &value));
    // Pin slot 1 as a stalled reader would. Writer may publish slot 0 once,
    // but must not overwrite the pinned slot when it laps that reader.
    atomic_fetch_or(&buffer.u_state, 4u);
    value = make_value(2); CHECK(TestBuffer_publish(&buffer, &value));
    value = make_value(3); CHECK(!TestBuffer_publish(&buffer, &value));
    CHECK(buffer.arr_slots[1].value.u64_seq == 1);
    atomic_fetch_and(&buffer.u_state, ~4u);
    CHECK(TestBuffer_publish(&buffer, &value));
    CHECK(TestBuffer_take(&buffer, &out) && out.u64_seq == 3);
    CHECK(!TestBuffer_take(&buffer, &out) && out.u64_seq == 3);
    puts("PASS buffer empty/coalescing/pinned-reader/reuse");

    pthread_t writer, reader;
    TestRing_init(&ring);
    CHECK(pthread_create(&writer, NULL, ring_writer, NULL) == 0);
    CHECK(pthread_create(&reader, NULL, ring_reader, NULL) == 0);
    CHECK(pthread_join(writer, NULL) == 0 && pthread_join(reader, NULL) == 0);
    printf("PASS ring concurrent transfers=%u\n", TRANSFERS);
    TestBuffer_init(&buffer);
    atomic_init(&b_done, false);
    CHECK(pthread_create(&writer, NULL, buffer_writer, NULL) == 0);
    CHECK(pthread_create(&reader, NULL, buffer_reader, NULL) == 0);
    CHECK(pthread_join(writer, NULL) == 0 && pthread_join(reader, NULL) == 0);
    printf("PASS buffer concurrent publications=%u (coherent, monotonic, final delivered)\n", TRANSFERS);
    return 0;
}
