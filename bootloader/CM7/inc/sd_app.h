/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sd_app.h
 * @brief SD card bring-up for the N32H787 HMI V1.1 board (CM7).
 *
 * SDMMC1, 4-bit bus, 3.3 V signalling. The host wrapper is configured by
 * SDMMC_Configuration() in n32h7xx_cfg.c; this module adds what that does not
 * do: the SDMMC1 kernel clock, the card identification sequence and a read-only
 * block test.
 */
#ifndef __SD_APP_H__
#define __SD_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "sd_host.h" /* sd_card_t, SD_ModeConfig */

/**
 * @brief Full bring-up: PA9 as USART1_TX, SDMMC1 clock, card identification,
 *        then a read-only block test. Reports over the NSLINK virtual COM port
 *        at 115200 8N1.
 */
void SD_App_Run(void);

/**
 * @brief The card identified by SD_App_Init(), for callers that drive it
 *        directly. Only meaningful once SD_App_Init() has returned true.
 */
sd_card_t *SD_App_Card(void);

/**
 * @brief CMD6-switch the bus timing and reprogram the bus clock.
 *
 * CMD6 has to be issued while the bus is still at default speed, so the timing
 * switch happens before the clock changes.
 *
 * @param mode  SDMMC_DS, SDMMC_HS, ...
 * @param hz    requested bus clock
 * @return the actual bus clock in Hz, or 0 if the card rejected the mode.
 */
uint32_t SD_App_ConfigureBus(SD_ModeConfig mode, uint32_t hz);

/* Individual steps, for driving the sequence yourself. */

/** Mux PA9 to USART1_TX so printf() has somewhere to go. */
void SD_App_DebugInit(void);

/** Configure the SDMMC1 clock and identify the inserted card. */
bool SD_App_Init(void);

/** Print the card's CID/CSD and read back a few blocks. */
void SD_App_Test(void);

#ifdef __cplusplus
}
#endif

#endif /* __SD_APP_H__ */
