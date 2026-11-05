/**
 * @file    ring_buffer.h
 * @brief   Lock-free single-producer / single-consumer ring buffer of 16-bit samples.
 *
 * Thread-safety contract
 * ----------------------
 * - Exactly ONE producer (here: the ADC ISR) calls rb_push().
 * - Exactly ONE consumer (here: the processing task) calls rb_pop() / rb_read().
 * - Under this contract no mutex and no critical section are needed:
 *     * `head` is written only by the producer, `tail` only by the consumer.
 *     * Both indices are aligned 32-bit words, so on Cortex-M a load or store
 *       of an index is a single LDR/STR and can never be seen half-written.
 *     * A memory barrier orders "write slot -> publish head" in the producer and
 *       "read head -> read slot -> release tail" in the consumer.
 * - The indices run freely (they are never wrapped). The slot is `index & mask`,
 *   and the fill level is `head - tail`. Unsigned overflow of the indices is
 *   harmless because the size is a power of two (it divides 2^32).
 *   This also allows all `size` slots to be used (no "one empty slot" rule).
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    volatile uint32_t head;        /**< Next slot to write. Written only by the producer. */
    volatile uint32_t tail;        /**< Next slot to read. Written only by the consumer.  */
    volatile uint32_t dropped;     /**< Pushes rejected because the buffer was full (producer). */
    volatile uint32_t high_water;  /**< Highest fill level seen (producer). */
    uint16_t         *data;        /**< Storage of `mask + 1` samples. */
    uint32_t          mask;        /**< size - 1 */
} ring_buffer_t;

/**
 * @brief  Initialise an empty buffer on top of caller-provided storage.
 * @param  size  Number of elements of `storage`. Must be a power of two (>= 2).
 * @retval false if the arguments are invalid.
 */
bool rb_init(ring_buffer_t *rb, uint16_t *storage, uint32_t size);

/**
 * @brief  Producer side: append one sample.
 * @retval false if the buffer is full. The sample is dropped and `dropped` is incremented.
 */
bool rb_push(ring_buffer_t *rb, uint16_t value);

/**
 * @brief  Consumer side: remove the oldest sample.
 * @retval false if the buffer is empty.
 */
bool rb_pop(ring_buffer_t *rb, uint16_t *value);

/**
 * @brief  Consumer side: remove up to `max` samples in one call.
 *         Reads `head` once and writes `tail` once, so it is cheaper than a pop loop.
 * @return Number of samples copied to `dst`.
 */
uint32_t rb_read(ring_buffer_t *rb, uint16_t *dst, uint32_t max);

/** @brief Current fill level (a snapshot; either side may call it). */
uint32_t rb_count(const ring_buffer_t *rb);

/** @brief Capacity in samples. */
uint32_t rb_capacity(const ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
