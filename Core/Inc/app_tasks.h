/**
 * @file    app_tasks.h
 * @brief   Application tasks of the DAQ project.
 *
 * The tasks are declared in CubeMX (Middleware > FREERTOS > Tasks and Queues)
 * with "Code Generation Option: As external". CubeMX creates them with
 * osThreadNew() in main.c, and the function bodies are implemented here.
 * (CubeMX still emits a __weak placeholder for the first task in main.c;
 * the definition in app_tasks.c overrides it at link time.)
 *
 *   Task           Entry function       Priority             Stack
 *   dataProcTask   DataProcessingTask   osPriorityAboveNormal 256 words
 *   reportTask     ReportTask           osPriorityLow         512 words
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

/** @brief Create the report queue. Call once, before the scheduler starts. */
void app_tasks_init(void);

/** @brief Waits for batches from the ADC ISR, drains the ring buffer, computes window statistics. */
void DataProcessingTask(void *argument);

/** @brief Prints the statistics over USART3 (ST-LINK virtual COM port) and drives the LEDs. */
void ReportTask(void *argument);

#endif /* APP_TASKS_H */
