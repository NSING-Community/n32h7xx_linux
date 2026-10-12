/* SPDX-License-Identifier: BSD-3-Clause */
#if defined(BOOT_SD_COMPARE)
#include <stdio.h>
#include "sd_app.h"
#include "mmc_host.h"
#include "sd_host.h"

/* Same 64 KiB transfers and first 64 MiB as Linux's O_DIRECT test. */
void SD_Compare_Run(void)
{
    uint32_t *buf = (uint32_t *)0xc0800000U;
    sd_card_t *card = SD_App_Card();
    uint32_t hz, pass, block, start, ms, rate;

    hz = SD_App_ConfigureBus(SDMMC_HS, 50000000U);
    if (hz != 50000000U) {
        printf("SD_COMPARE_FAIL clock=%lu\r\n", (unsigned long)hz);
        return;
    }
    printf("SD_COMPARE clock=%lu chunk=65536 bytes=67108864 dma=%u CCR=%08lx\r\n",
           (unsigned long)hz, (unsigned)card->card_workmode.dma,
           (unsigned long)SCB->CCR);
    printf("SD_REG ctrl1=%08lx ctrl2=%08lx pll2c1=%08lx pll2c2=%08lx pll2div=%08lx\r\n",
           (unsigned long)*(volatile uint32_t *)0x51110028U,
           (unsigned long)*(volatile uint32_t *)0x5111002cU,
           (unsigned long)RCC->PLL2CTRL1, (unsigned long)RCC->PLL2CTRL2,
           (unsigned long)RCC->PLL2DIV);
    for (pass = 0; pass < 3; pass++) {
        start = MMC_systick_timeoutms;
        for (block = 0; block < 131072U; block += 128U) {
            if (SD_ReadBlocks(card, buf, block, 128U) != Status_Success) {
                printf("SD_COMPARE_FAIL block=%lu\r\n", (unsigned long)block);
                return;
            }
        }
        ms = MMC_systick_timeoutms - start;
        if (!ms) {
            printf("SD_COMPARE_FAIL zero timer\r\n");
            return;
        }
        rate = 64000U / ms;
        printf("SD_READ pass=%lu ms=%lu MiB/s=%lu.%03lu last=%08lx\r\n",
               (unsigned long)pass, (unsigned long)ms, (unsigned long)rate,
               (unsigned long)((64000000U / ms) % 1000U), (unsigned long)buf[0]);
    }
    printf("SD_COMPARE_PASS\r\n");
}
#endif
