/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sd_bench.h
 * @brief SD card throughput benchmark (CM7, SDMMC1, 4-bit).
 */
#ifndef __SD_BENCH_H__
#define __SD_BENCH_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Measure sustained multi-block read/write bandwidth, at default speed
 *        and - if the card accepts the CMD6 switch - at high speed.
 *
 * DESTRUCTIVE: overwrites a 4 MiB region of the card. Meant to be called after
 * SD_App_Init() has succeeded; leaves the card back at default speed.
 */
void SD_Bench_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* __SD_BENCH_H__ */
