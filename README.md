# RTOS Data Acquisition Driver for STM32F412 (FreeRTOS)

An interrupt-driven analog data acquisition driver for the **NUCLEO-F412ZG** board,
built with **FreeRTOS**, **STM32CubeMX** and **STM32CubeIDE**.

The board samples the voltage on pin **A0** at a fixed rate (1000 samples per second
by default). Once per second it prints the minimum, average and maximum voltage over
the ST-LINK USB serial port.

## Contents

- [Features](#features)
- [What you need](#what-you-need)
- [Wiring](#wiring)
- [Quick start](#quick-start)
- [Reading the output](#reading-the-output)
- [Configuration](#configuration)
- [Changing the hardware setup in CubeMX](#changing-the-hardware-setup-in-cubemx)
- [How it works](#how-it-works)
- [Project structure](#project-structure)
- [Unit tests](#unit-tests)
- [Troubleshooting](#troubleshooting)
- [Possible extensions](#possible-extensions)

## Features

- **Fixed sample rate:** TIM2 triggers every ADC conversion in hardware, from 1 Hz to 20 kHz.
- **Short ISR:** the ADC interrupt only reads the sample, stores it and signals a task.
- **Lock-free ring buffer:** samples pass from the ISR to a task without a mutex.
- **Deferred interrupt processing:** a FreeRTOS binary semaphore and `portYIELD_FROM_ISR()` hand the work to a task.
- **Error detection:** counters for buffer overflow, ADC overrun and stalls, plus status LEDs.
- **Regenerates safely:** all application code is in its own files or in `USER CODE` sections.
- **Tested:** host unit tests cover the ring buffer.

## What you need

**Hardware**

- NUCLEO-F412ZG board and a micro-USB cable
- An analog source between 0 and 3.3 V, for example a 10 kΩ potentiometer or a signal generator

**Software**

- [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html) to build, flash and debug (built with version 2.2.0)
- [STM32CubeMX](https://www.st.com/en/development-tools/stm32cubemx.html), only if you want to change the peripheral setup (version 6.18)
- A serial terminal: the CubeIDE terminal, PuTTY, `screen` or `minicom`

## Wiring

| Board pin | Connect to |
|---|---|
| **A0** (PA3) | Signal to measure, for example the potentiometer wiper |
| **3V3** | One end of the potentiometer |
| **GND** | Other end of the potentiometer (and the signal ground) |

The serial output and the power both use the board's ST-LINK USB connector, so no
extra wiring is needed for them.

> **Never apply more than 3.3 V or less than 0 V to A0.**

## Quick start

1. **Clone the repository**

   ```sh
   git clone https://github.com/akshaybachhu-53/rtos-daq-driver.git
   ```

2. **Import it into STM32CubeIDE**
   - Go to **File → Import → General → Existing Projects into Workspace → Next**.
   - Under **Select root directory**, choose the cloned folder.
   - Check that `rtos-daq-driver` is ticked, then click **Finish**.

3. **Build**
   - Go to **Project → Build Project**, or press **Ctrl+B**.
   - The console should end with `Build Finished. 0 errors, 0 warnings.`

4. **Flash and run**
   - Connect the board with the USB cable.
   - Go to **Run → Debug As → STM32 C/C++ Application**, then click **OK** in the launch dialog.
   - When the debugger stops at `main()`, press **Resume** (F8).
   - To run without the debugger, use **Run → Run As → STM32 C/C++ Application** instead.

5. **Open the serial port** with these settings: **115200 baud, 8 data bits, no parity, 1 stop bit**.

   | OS | Port |
   |---|---|
   | Linux | `/dev/ttyACM0`, for example `screen /dev/ttyACM0 115200` |
   | Windows | "STMicroelectronics STLink Virtual COM Port (COMx)" in Device Manager |
   | macOS | `/dev/tty.usbmodem*` |

6. **Press the black RESET button.** The start banner is printed once, and then one
   line every second.

## Reading the output

```
RTOS DAQ driver | ADC1 IN3 (A0) | 1000 Hz | batch 16 | buffer 256 samples
t=1016 ms n=1000 min=1642 avg=1650 max=1659 mV | total=1008 dropped=0 ovr=0 stalls=0 peak=17/256
t=2016 ms n=1000 min=1640 avg=1651 max=1661 mV | total=2008 dropped=0 ovr=0 stalls=0 peak=17/256
```

These are example values. Yours depend on the voltage at A0.

| Field | Meaning |
|---|---|
| `t` | Time since reset, in ms |
| `n` | Number of samples in this one-second window |
| `min` / `avg` / `max` | Lowest, average and highest voltage in the window, in mV |
| `total` | Samples converted since reset |
| `dropped` | Samples lost because the ring buffer was full |
| `ovr` | ADC overruns: a new result arrived before the previous one was read |
| `stalls` | Restarts of the acquisition because no data arrived for 100 ms |
| `peak` | Highest fill level of the ring buffer / its capacity |

**Quick check:**

- A0 connected to GND should read about 0 mV.
- A0 connected to 3V3 should read about 3300 mV.
- A0 left unconnected reads random, noisy values, which is normal.

**LEDs**

| LED | Meaning |
|---|---|
| LD1 green | Toggles once per report, showing the system is running |
| LD3 red | On while `dropped`, `ovr` or `stalls` is non-zero (some data was lost) |
| LD3 red on, green stopped | Fatal error, the program is stopped in `Error_Handler()` (see [Troubleshooting](#troubleshooting)) |

## Configuration

All settings are in [`Core/Inc/daq_config.h`](Core/Inc/daq_config.h). Change a value and rebuild.

| Setting | Default | Meaning |
|---|---|---|
| `DAQ_SAMPLE_RATE_HZ` | 1000 | Samples per second (1 to `DAQ_MAX_SAMPLE_RATE_HZ`) |
| `DAQ_MAX_SAMPLE_RATE_HZ` | 20000 | Highest rate that `daq_start()` accepts |
| `DAQ_BATCH_SIZE` | 16 | Samples per task wake-up: fewer wake-ups, but more latency |
| `DAQ_RING_BUFFER_SIZE` | 256 | Buffer capacity. Must be a power of two |
| `DAQ_REPORT_PERIOD_MS` | 1000 | Length of one statistics window and report |
| `DAQ_STALL_TIMEOUT_MS` | 100 | Restart the acquisition if no batch arrives within this time |
| `DAQ_VREF_MV` | 3300 | ADC reference voltage used for the mV conversion |

The timer counts at 1 MHz, so the real sample rate is `1,000,000 / round_down(1,000,000 / rate)`.
Pick a rate that divides 1,000,000 exactly (for example 500, 1000, 2000, 5000 or 10000)
to get it exactly.

## Changing the hardware setup in CubeMX

1. Open [`rtos-daq-driver.ioc`](rtos-daq-driver.ioc) in STM32CubeMX.
2. Make your changes, then click **Generate Code**.
3. Rebuild in STM32CubeIDE.

The application code is kept, because it is in separate files or inside `USER CODE` blocks.

Keep these two ADC settings, or the driver will stop working:

- **NVIC → ADC1 global interrupt: priority 5 or higher number (5 to 15).**
  Interrupts that call FreeRTOS functions must not be more urgent than
  `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (5).
- **NVIC → Code generation → ADC1 global interrupt → "Generate IRQ handler" unticked.**
  The driver provides its own `ADC_IRQHandler` in `daq.c`.

Main CubeMX settings:

| Peripheral | Settings |
|---|---|
| RCC | HSE bypass 8 MHz (from the ST-LINK), PLL → 100 MHz system clock |
| ADC1 | IN3 (PA3), 12-bit, single conversion, external trigger TIM2 TRGO (rising edge), 84-cycle sampling |
| TIM2 | Prescaler 99 (1 MHz), period 999 (1 kHz), TRGO on update event |
| USART3 | 115200 8N1 on PD8/PD9 (ST-LINK virtual COM port) |
| SYS | HAL timebase on TIM6, because FreeRTOS uses SysTick |
| FREERTOS | CMSIS_V2, tasks `dataProcTask` (AboveNormal) and `reportTask` (Low), stack overflow check enabled |

## How it works

```
  TIM2 (1 kHz trigger)
        |
        v
  ADC1 on A0 --EOC interrupt--> ADC_IRQHandler
                                  - read sample (clears the flag)
                                  - push it into the ring buffer
                                  - every 16 samples: give semaphore,
                                    portYIELD_FROM_ISR()
                                        |
                                        v
                                DataProcessingTask (high priority)
                                  - waits on the semaphore (no CPU used)
                                  - drains the ring buffer
                                  - min / avg / max per 1 s window
                                        |  FreeRTOS queue
                                        v
                                ReportTask (low priority)
                                  - prints one line over USART3
                                  - drives the LEDs
```

### Design notes

- **Why a timer trigger and not continuous mode?** In continuous mode the ADC
  converts as fast as it can. That would mean one interrupt every few hundred
  nanoseconds, and no defined sample rate. The timer gives an exact rate and a
  bounded interrupt load.
- **Why is the ring buffer safe without a mutex?** There is exactly one writer of
  each index: the ISR writes `head`, the task writes `tail`. Each index is a 32-bit
  word, so it is read and written in one instruction. A memory barrier (`__DMB()`)
  makes sure a sample is stored before `head` publishes it. `volatile` alone would
  not guarantee that order. Because the size is a power of two, all slots can be used
  and the indices may overflow safely.
- **Why batch the wake-ups?** Giving the semaphore every 16 samples instead of every
  sample cuts context switches by a factor of 16. A binary semaphore does not count,
  so the task always empties the whole buffer when it wakes up.
- **Why two tasks?** Printing over the UART is slow. The printing runs in a
  lower-priority task, so it never delays the processing of new samples.
- **Static allocation:** the semaphore and the queue use static memory, so creating
  them cannot fail at run time.

## Project structure

```
rtos-daq-driver.ioc           STM32CubeMX project file
Core/
  Inc/daq_config.h            all settings
  Inc/ring_buffer.h           lock-free ring buffer (API and thread-safety rules)
  Src/ring_buffer.c
  Inc/daq.h                   ADC acquisition driver API
  Src/daq.c                   start/stop, ADC_IRQHandler, statistics
  Inc/app_tasks.h
  Src/app_tasks.c             DataProcessingTask and ReportTask
  Src/main.c                  generated by CubeMX, plus daq_init() and app_tasks_init()
  Src/freertos.c              generated by CubeMX, plus the FreeRTOS error hooks
  Src/stm32f4xx_it.c, ...     other generated files
Drivers/                      STM32F4 HAL and CMSIS (from STMicroelectronics)
Middlewares/                  FreeRTOS kernel and CMSIS-RTOS2 wrapper
Tests/                        host unit tests for the ring buffer
STM32F412ZGTX_FLASH.ld        linker script
```

## Unit tests

The ring buffer has no hardware dependency, so the same source file is also tested
on a PC with GCC:

```sh
make -C Tests
```

The tests cover:

- FIFO order
- empty and full handling
- reads that cross the end of the storage
- 32-bit index overflow
- a producer thread and a consumer thread passing 5,000,000 samples at the same time

Expected result: `All tests passed.`

## Troubleshooting

| Problem | What to check |
|---|---|
| No output in the terminal | Correct port and 115200 8N1. Press RESET, since the banner prints only once at start-up. On Linux, add your user to the `dialout` group if the port can't be opened. |
| Red LED on, green LED stopped blinking | The program is in `Error_Handler()`. Possible causes: a task stack overflow, the FreeRTOS heap is full, or `DAQ_SAMPLE_RATE_HZ` is out of range. Start a debug session and pause to see where it stopped. |
| Red LED on while the output continues | Some data was lost. `dropped > 0`: the buffer is too small for the rate, so increase `DAQ_RING_BUFFER_SIZE` or lower the rate. `ovr > 0`: the ADC interrupt was delayed too long. |
| `multiple definition of 'ADC_IRQHandler'` after regenerating | "Generate IRQ handler" was turned back on for the ADC in CubeMX. Untick it (see above). |
| Hangs in `configASSERT` after a CubeMX change | The ADC interrupt priority was set below 5. Set it back to 5 or higher. |
| Values always about 3300 mV or 0 mV | Check the wiring: A0 may be shorted to 3V3 or to GND. |

## Possible extensions

- DMA in circular mode, for much higher sample rates
- Several input channels in scan mode
- Direct-to-task notifications instead of the semaphore (faster, less RAM)
- Measure VREFINT to correct for the real supply voltage instead of assuming 3.3 V

## Third-party code

`Drivers/` contains the STM32F4 HAL and CMSIS from STMicroelectronics, and
`Middlewares/` contains the FreeRTOS kernel (MIT license). Both were added by
STM32CubeMX and keep their own license files.
