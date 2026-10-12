/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sdram_bench.h
 * @brief CPU-side SDRAM bandwidth on the N32H787 HMI V1.1 board (CM7).
 *
 * Answers "how fast can the core actually read and write the SDRAM it will be
 * running from", which is a different question from the SD card's throughput.
 * See sdram_bench.c for what is measured and what the numbers do and do not
 * include.
 */
#ifndef __SDRAM_BENCH_H__
#define __SDRAM_BENCH_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Time the CPU against SDRAM and against SRAM, and print the tables.
 *
 * Destructive to the first megabyte of SDRAM. Needs SDRAM_App_Config() to have
 * succeeded and USART1 to be up (SD_App_DebugInit()).
 */
void SDRAM_Bench_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* __SDRAM_BENCH_H__ */
