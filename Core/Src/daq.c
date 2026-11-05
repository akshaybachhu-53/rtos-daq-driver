/**
 * @file    daq.c
 * @brief   Interrupt-driven ADC data acquisition driver.
 *
 * CubeMX notes
 * - NVIC > ADC1 global interrupt: enabled, preemption priority 5, "Uses FreeRTOS
 *   functions" ticked, "Generate IRQ handler" UNticked. The handler below
 *   replaces the generated one, so a CubeMX re-generation does not overwrite it.
 * - Priority 5 matters: an ISR that calls a FreeRTOS "FromISR" API must have a
 *   priority numerically >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5).
 *   A more urgent priority (0..4) would corrupt the kernel (configASSERT traps it).
 */
#include "daq.h"

#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ring_buffer.h"

#if ((DAQ_RING_BUFFER_SIZE & (DAQ_RING_BUFFER_SIZE - 1U)) != 0U)
#error "DAQ_RING_BUFFER_SIZE must be a power of two"
#endif

#if (DAQ_BATCH_SIZE > (DAQ_RING_BUFFER_SIZE / 2U))
#error "DAQ_BATCH_SIZE must be at most half of DAQ_RING_BUFFER_SIZE"
#endif

/* Handles created by CubeMX in main.c */
extern ADC_HandleTypeDef hadc1;
extern TIM_HandleTypeDef htim2;

/* ---------------------------------------------------------------------------
 * Shared state
 *   ISR writes:  ring buffer head, s_samples, s_overruns, s_batch_fill
 *   Task writes: ring buffer tail
 * ------------------------------------------------------------------------- */
static uint16_t          s_storage[DAQ_RING_BUFFER_SIZE];
static ring_buffer_t     s_rb;

static StaticSemaphore_t s_sem_buffer;
static SemaphoreHandle_t s_data_ready;

static volatile uint32_t s_samples;
static volatile uint32_t s_overruns;
static uint32_t          s_batch_fill;      /* used only inside the ISR */

/* ADC power-up time tSTAB is 3 us max (datasheet). Same delay as the HAL. */
static void adc_stabilisation_delay(void)
{
    volatile uint32_t n = 3U * (SystemCoreClock / 1000000U);
    while (n > 0U)
    {
        n--;
    }
}

/* TIM2 is on APB1. Its kernel clock is PCLK1, doubled if the APB1 prescaler is not 1. */
static uint32_t tim2_clock_hz(void)
{
    const uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    return ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? pclk1 : (2U * pclk1);
}

void daq_init(void)
{
    (void)rb_init(&s_rb, s_storage, DAQ_RING_BUFFER_SIZE);

    s_samples    = 0U;
    s_overruns   = 0U;
    s_batch_fill = 0U;

    /* Statically allocated: no heap use, cannot fail at run time. Created empty. */
    s_data_ready = xSemaphoreCreateBinaryStatic(&s_sem_buffer);
    configASSERT(s_data_ready != NULL);
}

bool daq_start(uint32_t sample_rate_hz)
{
    /* CubeMX sets PSC = 99, so the counter runs at 1 MHz (100 MHz / 100). */
    const uint32_t tick_hz = tim2_clock_hz() / (htim2.Init.Prescaler + 1U);

    if ((sample_rate_hz == 0U) || (sample_rate_hz > DAQ_MAX_SAMPLE_RATE_HZ) ||
        ((tick_hz / sample_rate_hz) < 2U))
    {
        return false;
    }

    /* One update event (= one ADC trigger) every tick_hz / rate counts. */
    __HAL_TIM_SET_AUTORELOAD(&htim2, (tick_hz / sample_rate_hz) - 1U);
    __HAL_TIM_SET_COUNTER(&htim2, 0U);

    /* Clear stale flags, then enable end-of-conversion and overrun interrupts. */
    __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC | ADC_FLAG_OVR | ADC_FLAG_STRT);
    __HAL_ADC_ENABLE_IT(&hadc1, ADC_IT_EOC | ADC_IT_OVR);

    if ((hadc1.Instance->CR2 & ADC_CR2_ADON) == 0U)
    {
        __HAL_ADC_ENABLE(&hadc1);
        adc_stabilisation_delay();
    }

    /* From here on, every TRGO starts one conversion; no software trigger needed. */
    return (HAL_TIM_Base_Start(&htim2) == HAL_OK);
}

void daq_stop(void)
{
    (void)HAL_TIM_Base_Stop(&htim2);
    __HAL_ADC_DISABLE_IT(&hadc1, ADC_IT_EOC | ADC_IT_OVR);
    __HAL_ADC_DISABLE(&hadc1);
}

bool daq_wait_for_data(uint32_t timeout_ms)
{
    /* Blocked state: the task uses no CPU time until the ISR gives the semaphore. */
    return (xSemaphoreTake(s_data_ready, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

uint32_t daq_read(uint16_t *dst, uint32_t max)
{
    return rb_read(&s_rb, dst, max);
}

void daq_get_stats(daq_stats_t *stats)
{
    /* Masks interrupts up to configMAX_SYSCALL_INTERRUPT_PRIORITY (BASEPRI),
     * which includes the ADC IRQ, so all counters come from the same moment. */
    taskENTER_CRITICAL();
    stats->samples           = s_samples;
    stats->dropped           = s_rb.dropped;
    stats->adc_overruns      = s_overruns;
    stats->buffer_high_water = s_rb.high_water;
    taskEXIT_CRITICAL();
    stats->buffer_size       = rb_capacity(&s_rb);
}

/**
 * @brief ADC1 global interrupt. Kept as short as possible:
 *        read the sample, store it, and signal the task once per batch.
 *        All processing happens later in task context (deferred interrupt processing).
 */
void ADC_IRQHandler(void)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    const uint32_t sr = ADC1->SR;

    if ((sr & ADC_SR_OVR) != 0U)
    {
        /* SR flags are rc_w0: writing 0 clears a flag, writing 1 leaves it unchanged.
         * So ~OVR clears only OVR. The ADC converts again on the next trigger. */
        ADC1->SR = ~ADC_SR_OVR;
        s_overruns++;
    }

    if ((sr & ADC_SR_EOC) != 0U)
    {
        /* Reading DR clears EOC in hardware: no separate flag clear is needed. */
        const uint16_t sample = (uint16_t)(ADC1->DR & 0x0FFFU);

        (void)rb_push(&s_rb, sample);      /* if full, the buffer counts the drop */
        s_samples++;

        if (++s_batch_fill >= DAQ_BATCH_SIZE)
        {
            s_batch_fill = 0U;
            (void)xSemaphoreGiveFromISR(s_data_ready, &higher_priority_task_woken);
        }
    }

    /* If giving the semaphore unblocked a task with a higher priority than the one
     * that was interrupted, switch to it directly when the ISR returns (via PendSV)
     * instead of waiting for the next tick. */
    portYIELD_FROM_ISR(higher_priority_task_woken);
}
