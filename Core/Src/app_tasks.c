/**
 * @file    app_tasks.c
 * @brief   Processing and reporting tasks.
 *
 *   ADC ISR --(ring buffer + binary semaphore)--> DataProcessingTask
 *   DataProcessingTask --(queue of daq_report_t)--> ReportTask --> USART3
 *
 * DataProcessingTask runs at a higher priority than ReportTask, so slow UART
 * output never delays the draining of the ring buffer.
 */
#include "app_tasks.h"

#include <inttypes.h>
#include <stdio.h>

#include "main.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "daq.h"

#define ARRAY_LEN(a)          (sizeof(a) / sizeof((a)[0]))
#define REPORT_QUEUE_LENGTH   4U
#define UART_TX_TIMEOUT_MS    100U

/* UART handle created by CubeMX in main.c */
extern UART_HandleTypeDef huart3;

typedef struct
{
    uint32_t    time_ms;      /* end of the window, since boot */
    uint32_t    count;        /* samples in the window */
    uint16_t    min_mv;
    uint16_t    mean_mv;
    uint16_t    max_mv;
    uint32_t    stalls;       /* acquisition restarts after a timeout */
    daq_stats_t daq;          /* driver counters at the end of the window */
} daq_report_t;

typedef struct
{
    uint32_t count;
    uint32_t sum;             /* 4095 * 20000 samples still fits in 32 bits */
    uint16_t min;
    uint16_t max;
} window_t;

static StaticQueue_t s_report_queue_cb;
static uint8_t       s_report_queue_storage[REPORT_QUEUE_LENGTH * sizeof(daq_report_t)];
static QueueHandle_t s_report_queue;

void app_tasks_init(void)
{
    s_report_queue = xQueueCreateStatic(REPORT_QUEUE_LENGTH, sizeof(daq_report_t),
                                        s_report_queue_storage, &s_report_queue_cb);
    configASSERT(s_report_queue != NULL);
}

/* ------------------------------------------------------------------------- */

static void window_reset(window_t *w)
{
    w->count = 0U;
    w->sum   = 0U;
    w->min   = UINT16_MAX;
    w->max   = 0U;
}

static void window_add(window_t *w, uint16_t sample)
{
    w->count++;
    w->sum += sample;
    if (sample < w->min) { w->min = sample; }
    if (sample > w->max) { w->max = sample; }
}

static void publish_report(const window_t *w, uint32_t stalls)
{
    daq_report_t r;
    const uint32_t mean_raw = (w->sum + (w->count / 2U)) / w->count;

    r.time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    r.count   = w->count;
    r.min_mv  = (uint16_t)daq_raw_to_mv(w->min);
    r.mean_mv = (uint16_t)daq_raw_to_mv(mean_raw);
    r.max_mv  = (uint16_t)daq_raw_to_mv(w->max);
    r.stalls  = stalls;
    daq_get_stats(&r.daq);

    /* Never block the processing task: if the reporter is behind, skip this report. */
    (void)xQueueSend(s_report_queue, &r, 0U);
}

void DataProcessingTask(void *argument)
{
    (void)argument;

    const uint32_t window_samples = (DAQ_SAMPLE_RATE_HZ * DAQ_REPORT_PERIOD_MS) / 1000U;
    uint16_t block[2U * DAQ_BATCH_SIZE];
    window_t window;
    uint32_t stalls = 0U;

    window_reset(&window);

    if (!daq_start(DAQ_SAMPLE_RATE_HZ))
    {
        Error_Handler();
    }

    for (;;)
    {
        /* Sleep until the ISR signals a full batch. */
        if (!daq_wait_for_data(DAQ_STALL_TIMEOUT_MS))
        {
            /* No batch in time: the trigger or the ADC stopped. Restart the acquisition. */
            stalls++;
            daq_stop();
            (void)daq_start(DAQ_SAMPLE_RATE_HZ);
            continue;
        }

        /* A binary semaphore does not count: several "gives" while this task was busy
         * collapse into one. So drain everything in the buffer, not just one batch. */
        uint32_t n;
        while ((n = daq_read(block, ARRAY_LEN(block))) > 0U)
        {
            for (uint32_t i = 0U; i < n; i++)
            {
                window_add(&window, block[i]);
                if (window.count >= window_samples)
                {
                    publish_report(&window, stalls);
                    window_reset(&window);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */

static void uart_write(const char *text, int len)
{
    if (len <= 0)
    {
        return;
    }
    (void)HAL_UART_Transmit(&huart3, (const uint8_t *)text, (uint16_t)len, UART_TX_TIMEOUT_MS);
}

void ReportTask(void *argument)
{
    (void)argument;

    char line[200];
    daq_report_t r;
    int len;

    len = snprintf(line, sizeof(line),
                   "\r\nRTOS DAQ driver | ADC1 IN3 (A0) | %u Hz | batch %u | buffer %u samples\r\n",
                   (unsigned)DAQ_SAMPLE_RATE_HZ, (unsigned)DAQ_BATCH_SIZE,
                   (unsigned)DAQ_RING_BUFFER_SIZE);
    uart_write(line, (len < (int)sizeof(line)) ? len : (int)sizeof(line) - 1);

    for (;;)
    {
        if (xQueueReceive(s_report_queue, &r, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        len = snprintf(line, sizeof(line),
                       "t=%" PRIu32 " ms n=%" PRIu32 " min=%u avg=%u max=%u mV"
                       " | total=%" PRIu32 " dropped=%" PRIu32 " ovr=%" PRIu32
                       " stalls=%" PRIu32 " peak=%" PRIu32 "/%" PRIu32 "\r\n",
                       r.time_ms, r.count,
                       (unsigned)r.min_mv, (unsigned)r.mean_mv, (unsigned)r.max_mv,
                       r.daq.samples, r.daq.dropped, r.daq.adc_overruns,
                       r.stalls, r.daq.buffer_high_water, r.daq.buffer_size);
        uart_write(line, (len < (int)sizeof(line)) ? len : (int)sizeof(line) - 1);

        /* Green LED: heartbeat, toggles once per report.
         * Red LED: on once any sample was lost (buffer full, ADC overrun or stall). */
        HAL_GPIO_TogglePin(LD1_GPIO_Port, LD1_Pin);
        const bool data_lost = (r.daq.dropped != 0U) || (r.daq.adc_overruns != 0U) || (r.stalls != 0U);
        HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, data_lost ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}
