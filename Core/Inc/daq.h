/**
 * @file    daq.h
 * @brief   Interrupt-driven ADC data acquisition driver (ADC1 + TIM2 trigger + FreeRTOS).
 *
 * Data path
 *   TIM2 update (TRGO) -> ADC1 conversion -> ADC_IRQHandler (EOC)
 *     -> ring buffer push -> every DAQ_BATCH_SIZE samples: xSemaphoreGiveFromISR()
 *     -> processing task wakes in daq_wait_for_data() and drains with daq_read().
 *
 * The peripherals are configured by CubeMX (MX_ADC1_Init, MX_TIM2_Init).
 * This driver only starts/stops them and owns the interrupt handler.
 */
#ifndef DAQ_H
#define DAQ_H

#include <stdbool.h>
#include <stdint.h>

#include "daq_config.h"

typedef struct
{
    uint32_t samples;            /**< Conversions completed since daq_init(). */
    uint32_t dropped;            /**< Samples lost because the ring buffer was full. */
    uint32_t adc_overruns;       /**< ADC OVR events: a result was overwritten before it was read. */
    uint32_t buffer_high_water;  /**< Highest ring buffer fill level seen. */
    uint32_t buffer_size;        /**< Ring buffer capacity. */
} daq_stats_t;

/** @brief Create the ring buffer and the binary semaphore. Call once, before the scheduler starts. */
void daq_init(void);

/**
 * @brief  Program TIM2 for `sample_rate_hz`, enable ADC1 and its EOC/OVR interrupts, start TIM2.
 * @retval false if the rate is out of range (see DAQ_MAX_SAMPLE_RATE_HZ).
 */
bool daq_start(uint32_t sample_rate_hz);

/** @brief Stop the trigger timer and the ADC. */
void daq_stop(void);

/**
 * @brief  Block the calling task until the ISR signals a new batch.
 * @retval false on timeout.
 */
bool daq_wait_for_data(uint32_t timeout_ms);

/** @brief Copy up to `max` samples out of the ring buffer. Single consumer only. */
uint32_t daq_read(uint16_t *dst, uint32_t max);

/** @brief Consistent snapshot of the driver counters. */
void daq_get_stats(daq_stats_t *stats);

/** @brief Convert a 12-bit ADC code to millivolts (rounded). */
static inline uint32_t daq_raw_to_mv(uint32_t raw)
{
    return ((raw * DAQ_VREF_MV) + (DAQ_ADC_FULL_SCALE / 2U)) / DAQ_ADC_FULL_SCALE;
}

#endif /* DAQ_H */
