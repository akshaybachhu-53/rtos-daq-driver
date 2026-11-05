/**
 * @file    daq_config.h
 * @brief   Compile-time settings of the data acquisition driver.
 *
 * All tuning values of the project are in this file.
 */
#ifndef DAQ_CONFIG_H
#define DAQ_CONFIG_H

/* Sampling rate in Hz. TIM2 generates one ADC trigger (TRGO) per sample. */
#define DAQ_SAMPLE_RATE_HZ        1000U

/* Highest sampling rate accepted by daq_start(). */
#define DAQ_MAX_SAMPLE_RATE_HZ    20000U

/* The ISR wakes the processing task once every DAQ_BATCH_SIZE samples.
 * A larger batch means fewer context switches but a longer latency. */
#define DAQ_BATCH_SIZE            16U

/* Capacity of the ring buffer in samples. Must be a power of two.
 * 256 samples = 256 ms of data at 1 kHz. */
#define DAQ_RING_BUFFER_SIZE      256U

/* Length of one statistics window. One report is printed per window. */
#define DAQ_REPORT_PERIOD_MS      1000U

/* If no batch arrives within this time, the acquisition is restarted. */
#define DAQ_STALL_TIMEOUT_MS      100U

/* Analog reference (VDDA = 3.3 V on the Nucleo board) and 12-bit full scale. */
#define DAQ_VREF_MV               3300U
#define DAQ_ADC_FULL_SCALE        4095U

#endif /* DAQ_CONFIG_H */
