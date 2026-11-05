/**
 * @file    ring_buffer.c
 * @brief   Lock-free SPSC ring buffer. See ring_buffer.h for the thread-safety contract.
 *
 * Why `volatile` alone is not enough
 * ----------------------------------
 * `volatile` on head/tail forces every access to really go to memory: the
 * consumer re-reads `head` each time instead of keeping it in a register.
 * It does NOT order a volatile access against the plain accesses to `data[]`,
 * so the compiler could move the sample write after the head update.
 * RB_MEMORY_BARRIER() prevents that:
 *   - On the target it is __DMB(), a compiler barrier ("memory" clobber) plus a
 *     data memory barrier. On a single-core Cortex-M4 only the compiler barrier
 *     is strictly required. The DMB costs a few cycles and keeps the code correct
 *     on cores or bus masters that do reorder memory accesses.
 *   - On the host (unit tests) it is a full C11 fence.
 */
#include "ring_buffer.h"

#include <stddef.h>

#if defined(__ARM_ARCH)
#include "cmsis_compiler.h"
#define RB_MEMORY_BARRIER()   __DMB()
#else
#define RB_MEMORY_BARRIER()   __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static bool is_power_of_two(uint32_t x)
{
    return (x != 0U) && ((x & (x - 1U)) == 0U);
}

bool rb_init(ring_buffer_t *rb, uint16_t *storage, uint32_t size)
{
    if ((rb == NULL) || (storage == NULL) || (size < 2U) || !is_power_of_two(size))
    {
        return false;
    }

    rb->data       = storage;
    rb->mask       = size - 1U;
    rb->head       = 0U;
    rb->tail       = 0U;
    rb->dropped    = 0U;
    rb->high_water = 0U;
    return true;
}

bool rb_push(ring_buffer_t *rb, uint16_t value)
{
    const uint32_t head = rb->head;          /* own index */
    const uint32_t tail = rb->tail;          /* consumer index: one atomic 32-bit load */
    const uint32_t used = head - tail;       /* correct across 2^32 wrap-around */

    if (used > rb->mask)                     /* used == size: full */
    {
        rb->dropped++;                       /* only the producer writes this counter */
        return false;
    }

    /* The slot is free: the consumer released it before it advanced `tail`. */
    rb->data[head & rb->mask] = value;

    RB_MEMORY_BARRIER();                     /* sample is in memory before it is published */
    rb->head = head + 1U;                    /* single store publishes the sample */

    if ((used + 1U) > rb->high_water)
    {
        rb->high_water = used + 1U;
    }
    return true;
}

bool rb_pop(ring_buffer_t *rb, uint16_t *value)
{
    return (rb_read(rb, value, 1U) == 1U);
}

uint32_t rb_read(ring_buffer_t *rb, uint16_t *dst, uint32_t max)
{
    const uint32_t tail      = rb->tail;     /* own index */
    const uint32_t head      = rb->head;     /* producer index: one atomic 32-bit load */
    const uint32_t available = head - tail;
    const uint32_t n         = (available < max) ? available : max;

    if (n == 0U)
    {
        return 0U;
    }

    RB_MEMORY_BARRIER();                     /* read `head` before reading the slots it covers */

    for (uint32_t i = 0U; i < n; i++)
    {
        dst[i] = rb->data[(tail + i) & rb->mask];
    }

    RB_MEMORY_BARRIER();                     /* finish reading before the slots are released */
    rb->tail = tail + n;                     /* single store releases n slots to the producer */
    return n;
}

uint32_t rb_count(const ring_buffer_t *rb)
{
    const uint32_t tail = rb->tail;
    const uint32_t head = rb->head;
    return head - tail;
}

uint32_t rb_capacity(const ring_buffer_t *rb)
{
    return rb->mask + 1U;
}
