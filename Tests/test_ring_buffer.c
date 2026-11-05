/**
 * @file    test_ring_buffer.c
 * @brief   Host unit tests for Core/Src/ring_buffer.c (run with: make -C Tests).
 *
 * The same source file that runs on the STM32 is compiled for the PC.
 * The last test runs a producer thread and a consumer thread at the same time
 * to exercise the lock-free single-producer / single-consumer contract.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "ring_buffer.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_init_rejects_bad_arguments(void)
{
    ring_buffer_t rb;
    uint16_t storage[8];

    CHECK(!rb_init(&rb, storage, 0U));
    CHECK(!rb_init(&rb, storage, 1U));
    CHECK(!rb_init(&rb, storage, 6U));          /* not a power of two */
    CHECK(!rb_init(&rb, NULL, 8U));
    CHECK(rb_init(&rb, storage, 8U));
    CHECK(rb_capacity(&rb) == 8U);
    CHECK(rb_count(&rb) == 0U);
}

static void test_empty_pop_fails(void)
{
    ring_buffer_t rb;
    uint16_t storage[4];
    uint16_t v = 0xBEEFU;

    CHECK(rb_init(&rb, storage, 4U));
    CHECK(!rb_pop(&rb, &v));
    CHECK(v == 0xBEEFU);                         /* output untouched */
    CHECK(rb_read(&rb, &v, 1U) == 0U);
}

static void test_fifo_order(void)
{
    ring_buffer_t rb;
    uint16_t storage[8];
    uint16_t v;

    CHECK(rb_init(&rb, storage, 8U));
    for (uint16_t i = 0U; i < 5U; i++)
    {
        CHECK(rb_push(&rb, (uint16_t)(100U + i)));
    }
    CHECK(rb_count(&rb) == 5U);
    for (uint16_t i = 0U; i < 5U; i++)
    {
        CHECK(rb_pop(&rb, &v));
        CHECK(v == (uint16_t)(100U + i));
    }
    CHECK(rb_count(&rb) == 0U);
}

static void test_full_uses_every_slot_and_counts_drops(void)
{
    ring_buffer_t rb;
    uint16_t storage[4];
    uint16_t v;

    CHECK(rb_init(&rb, storage, 4U));
    for (uint16_t i = 0U; i < 4U; i++)
    {
        CHECK(rb_push(&rb, i));                  /* all 4 slots usable */
    }
    CHECK(!rb_push(&rb, 99U));                   /* full */
    CHECK(!rb_push(&rb, 98U));
    CHECK(rb.dropped == 2U);
    CHECK(rb.high_water == 4U);

    CHECK(rb_pop(&rb, &v) && (v == 0U));         /* oldest data kept, newest dropped */
    CHECK(rb_push(&rb, 4U));                     /* one slot free again */
    for (uint16_t i = 1U; i <= 4U; i++)
    {
        CHECK(rb_pop(&rb, &v) && (v == i));
    }
}

static void test_bulk_read_wraps_around_storage(void)
{
    ring_buffer_t rb;
    uint16_t storage[8];
    uint16_t out[8];

    CHECK(rb_init(&rb, storage, 8U));
    for (uint16_t i = 0U; i < 6U; i++) { (void)rb_push(&rb, i); }
    CHECK(rb_read(&rb, out, 6U) == 6U);          /* head = tail = 6 */

    for (uint16_t i = 0U; i < 7U; i++) { CHECK(rb_push(&rb, (uint16_t)(10U + i))); }
    CHECK(rb_read(&rb, out, 3U) == 3U);          /* partial read */
    CHECK((out[0] == 10U) && (out[1] == 11U) && (out[2] == 12U));
    CHECK(rb_read(&rb, out, 8U) == 4U);          /* rest, crossing the end of storage */
    CHECK((out[0] == 13U) && (out[3] == 16U));
}

static void test_32bit_index_wrap(void)
{
    ring_buffer_t rb;
    uint16_t storage[16];
    uint16_t v;

    CHECK(rb_init(&rb, storage, 16U));
    rb.head = UINT32_MAX - 5U;                   /* indices about to overflow */
    rb.tail = UINT32_MAX - 5U;

    for (uint16_t i = 0U; i < 16U; i++) { CHECK(rb_push(&rb, i)); }
    CHECK(rb.head < rb.tail);                    /* head has wrapped past 2^32 */
    CHECK(rb_count(&rb) == 16U);
    CHECK(!rb_push(&rb, 0U));                    /* still detected as full */
    for (uint16_t i = 0U; i < 16U; i++) { CHECK(rb_pop(&rb, &v) && (v == i)); }
    CHECK(rb_count(&rb) == 0U);
}

/* --- concurrent producer / consumer ------------------------------------- */

#define STRESS_SAMPLES 5000000U

static ring_buffer_t s_stress_rb;
static uint16_t      s_stress_storage[256];

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0U; i < STRESS_SAMPLES; )
    {
        if (rb_push(&s_stress_rb, (uint16_t)i)) { i++; }   /* retry while full */
    }
    return NULL;
}

static void *consumer(void *arg)
{
    uint32_t *errors = (uint32_t *)arg;
    uint16_t block[32];
    uint32_t expected = 0U;

    while (expected < STRESS_SAMPLES)
    {
        const uint32_t n = rb_read(&s_stress_rb, block, 32U);
        for (uint32_t i = 0U; i < n; i++, expected++)
        {
            if (block[i] != (uint16_t)expected) { (*errors)++; }
        }
    }
    return NULL;
}

static void test_concurrent_spsc(void)
{
    pthread_t p, c;
    uint32_t errors = 0U;

    CHECK(rb_init(&s_stress_rb, s_stress_storage, 256U));
    CHECK(pthread_create(&c, NULL, consumer, &errors) == 0);
    CHECK(pthread_create(&p, NULL, producer, NULL) == 0);
    (void)pthread_join(p, NULL);
    (void)pthread_join(c, NULL);

    CHECK(errors == 0U);                         /* every sample arrived, in order */
    CHECK(rb_count(&s_stress_rb) == 0U);
    printf("  concurrent: %u samples, %u errors, peak fill %u/256\n",
           (unsigned)STRESS_SAMPLES, (unsigned)errors, (unsigned)s_stress_rb.high_water);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "init rejects bad arguments",          test_init_rejects_bad_arguments },
        { "empty pop fails",                     test_empty_pop_fails },
        { "FIFO order",                          test_fifo_order },
        { "full buffer uses every slot",         test_full_uses_every_slot_and_counts_drops },
        { "bulk read wraps around storage",      test_bulk_read_wraps_around_storage },
        { "32-bit index wrap-around",            test_32bit_index_wrap },
        { "concurrent producer/consumer",        test_concurrent_spsc },
    };

    for (size_t i = 0U; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        const int before = s_failures;
        tests[i].fn();
        printf("[%s] %s\n", (s_failures == before) ? "PASS" : "FAIL", tests[i].name);
    }

    printf("%s\n", (s_failures == 0) ? "All tests passed." : "Some tests FAILED.");
    return (s_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
