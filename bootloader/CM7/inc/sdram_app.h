/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sdram_app.h
 * @brief SDRAM bring-up for the N32H787 HMI V1.1 board (CM7).
 *
 * U11 is an IS42S32800J-6BLI: 32 MiB, 4 banks x 4096 rows x 512 columns, x32,
 * CAS 3, burst 2, clocked at 133 MHz. See sdram_app.c for why this module
 * exists next to the N32Cube-generated SDRAM_Configuration().
 */
#ifndef __SDRAM_APP_H__
#define __SDRAM_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/** The whole device: 8M words x 32 bits. */
#define SDRAM_APP_BASE  (0xC0000000U)
#define SDRAM_APP_BYTES (32U * 1024U * 1024U)

/**
 * @brief Configure the SDRAM controller, run the 133 MHz initialisation
 *        sequence and self-test the whole device.
 *
 * Destructive: the self-test overwrites SDRAM. Call it after the clocks and
 * GPIO are set up and before anything is placed in SDRAM. Reports over USART1,
 * so SD_App_DebugInit() has to have run.
 *
 * @return true when the controller is up and the self-test passed.
 */
bool SDRAM_App_Config(void);

/**
 * @brief Result of the last SDRAM_App_Config().
 * @return SDRAM_APP_STATUS_* below.
 */
uint32_t SDRAM_App_Status(void);

#define SDRAM_APP_STATUS_READY    (0U)          /**< controller up, self-test passed */
#define SDRAM_APP_STATUS_CLOCK    (1U)          /**< PLL3A is not 133 MHz */
#define SDRAM_APP_STATUS_UNSAFE   (2U)          /**< D-cache on: not safe to touch SDRAM yet */
#define SDRAM_APP_STATUS_TEST     (3U)          /**< self-test found bad data */
#define SDRAM_APP_STATUS_UNUSED   (0xFFFFFFFFU) /**< never run */

/* The first word the self-test could not read back, and what it saw. */
extern volatile uint32_t g_sdram_error_address;
extern volatile uint32_t g_sdram_error_expected;
extern volatile uint32_t g_sdram_error_actual;

#ifdef __cplusplus
}
#endif

#endif /* __SDRAM_APP_H__ */
