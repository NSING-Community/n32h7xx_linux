/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sd_app.c
 * @brief SD card bring-up for the N32H787 HMI V1.1 board (CM7, SDMMC1, 4-bit).
 *
 * Ported from the vendor example
 * projects/n32h7xx_EVAL/examples/SDMMC/SDCard_Polling/src/SDModeConfig.c
 * (SD_PowerOnInit), whose readme validates this board with an SDXC card.
 *
 * Deliberate differences from that example:
 *   - The SDMMC1 kernel clock comes from PLL2A (100 MHz) instead of the
 *     AXIDIV/8 (37.5 MHz) that NTFx generates. See SD_App_Init().
 *   - No SDMMC_port_config(): the pins and alternate functions already match
 *     (PC12 AF1 = CK, PD2 AF2 = CMD, PC8 AF0 = D0, PC9..PC11 AF1 = D1..D3).
 *   - No AFIO_SDMMCClkSel(): SDMMC_Configuration() already does it.
 *   - No systick_delay_us(): the SDK's delay.c takes over SysTick and switches
 *     it off afterwards, which would kill the 1 ms tick from main().
 *   - No CMD11 / 1.8 V switching: this is a 3.3 V card.
 *   - Logging goes through the project's own _write() rather than the SDK BSP,
 *     which would also mux PA10 (USB1_ID on this board) as a UART pin.
 */

#include "sd_app.h"

#include <stdio.h>

#include "n32h7xx.h"
#include "n32h7xx_rcc.h"
#include "n32h7xx_gpio.h"
#include "n32h7xx_usart.h"
#include "n32h7xx_pwr.h"
#include "n32h7xx_sdmmc.h"
#include "sd_bench.h"
#include "sd_host.h"

/**
 * SDMMC1 kernel clock, established by SD_App_Init() from
 * HSE 25 MHz -> PLL2 500 MHz -> PLL2A /5.
 */
#define SD_APP_XIN_CLK (100000000U)

/**
 * Requested card bus clock. Deliberately below SD_APP_XIN_CLK: the vendor
 * prescaler helper compares SD_APP_XIN_CLK against this value in its fallback
 * branch, and a target >= SD_APP_XIN_CLK makes it return 0, which is not a
 * usable selector.
 */
#define SD_APP_BUS_CLK_HZ (25000000U)

/** Blocks read in the test (8 x 512 B = 4 KiB). */
#define SD_APP_TEST_BLOCKS (8U)
#define SD_APP_TEST_WORDS  (SD_APP_TEST_BLOCKS * FSL_SDMMC_DEFAULT_BLOCK_SIZE / 4U)

/** Card-detect / ACMD41 timeout, in milliseconds. */
#define SD_APP_CARD_TIMEOUT_MS (1000U)

static sd_card_t g_sd_card;

/* Static, not on the stack: 4 KiB each, and SD_ReadBlocks() hands the address
 * straight to the controller. */
static uint32_t g_sd_read_a[SD_APP_TEST_WORDS];
static uint32_t g_sd_read_b[SD_APP_TEST_WORDS];

/* -------------------------------------------------------------------------- */
/* Debug output                                                               */
/* -------------------------------------------------------------------------- */

void SD_App_DebugInit(void)
{
    GPIO_InitType GPIO_InitStructure;

    /* USART1 itself is already initialised and enabled by USART_Configuration()
     * in n32h7xx_cfg.c, and RCC_EnableAPB1PeriphClk3(RCC_APB1_PERIPHEN_M7_USART1)
     * is already done there too. The only missing piece is the pin mux.
     *
     * PA9 only. On this board PA10 is the USB1 OTG ID pin (net USB1_ID_PA10),
     * not USART1_RX, so it must not be muxed. PA9 (net NSLINK_MCU_TX_PA9) goes
     * to the on-board NSLINK probe's virtual COM port.
     *
     * GPIOA's clock is already enabled by GPIO_Configuration(). */
    GPIO_InitStruct(&GPIO_InitStructure);
    GPIO_InitStructure.Pin            = GPIO_PIN_9;
    GPIO_InitStructure.GPIO_Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStructure.GPIO_Pull      = GPIO_NO_PULL;
    GPIO_InitStructure.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    GPIO_InitStructure.GPIO_Current   = GPIO_HS_IO_DC_2mA;
    GPIO_InitStructure.GPIO_Alternate = GPIO_AF7; /* USART1_TX */
    GPIO_InitPeripheral(GPIOA, &GPIO_InitStructure);

    /* Unbuffered, so no stdio buffer has to come from the heap - _sbrk() is
     * stubbed out by --specs=nosys.specs. */
    setvbuf(stdout, NULL, _IONBF, 0);
}

#if !defined(BOOT_STORAGE_SPI_FLASH)
int _write(int fd, char *buf, int len)
{
    int i;

    (void)fd;

    for (i = 0; i < len; i++)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXDE) == RESET)
        {
        }
        /* This SDK names the transmit-empty flag TXDE, not TXE. */
        USART_SendData(USART1, (uint8_t)buf[i]);
    }

    return len;
}
#endif /* !BOOT_STORAGE_SPI_FLASH */

/* -------------------------------------------------------------------------- */
/* SDMMC1 setup and card identification                                       */
/* -------------------------------------------------------------------------- */

/**
 * Faithful port of the vendor prescaler arithmetic. Note that the fallback
 * branch compares SD_APP_XIN_CLK against busClock_Hz even when dividing for a
 * different target - reproduced as-is so the resulting clocks match the
 * validated example. The actual clock is SD_APP_XIN_CLK / (2 * prescaler).
 */
static uint32_t SD_App_Prescaler(uint32_t divisorHz, uint32_t busClock_Hz)
{
    if (((SD_APP_XIN_CLK % divisorHz) != 0U) || (((SD_APP_XIN_CLK / divisorHz) % 2U) != 0U))
    {
        if (SD_APP_XIN_CLK <= busClock_Hz)
        {
            return 0U;
        }
        return (SD_APP_XIN_CLK / divisorHz / 2U) + 1U;
    }

    return SD_APP_XIN_CLK / divisorHz / 2U;
}

/**
 * @brief Reconfigure the six SDMMC1 pins for high speed signalling.
 *
 * n32h7xx_cfg.c sets all six to no pull-up, 2 mA and slow slew - a default
 * speed configuration, and good enough that bring-up at 25 MHz works. Two of
 * those choices stop working once the bus clock goes up:
 *
 *  - no pull-up on CMD and DAT0-3. These are shared lines: the host holds them
 *    high while idle and either side pulls them down to drive a bit. With no
 *    pull-up the idle level is only the pad's own weak keeper, and the card can
 *    read the resulting slow rise as the start of a transfer. The clock is
 *    never pulled - it is push-pull and always driven.
 *  - slow slew at 2 mA on PC12, which drives the bus clock. Every other line is
 *    sampled against that edge, and at 50 MHz a bit is 20 ns, so a slow edge
 *    eats a large part of the setup margin.
 *
 * The pads differ in what they support, so each takes the encoding for its own
 * class: PC12 and PD2 are high speed pads, PC8 is 5 V tolerant, PC9-PC11 are
 * 3.3 V tolerant.
 */
static void SD_App_ConfigurePads(void)
{
    GPIO_InitType gpio;

    /* PC12 = SDMMC1.CK, PC9-PC11 = D1-D3, all AF1. */
    gpio.Pin            = GPIO_PIN_12;
    gpio.GPIO_Mode      = GPIO_MODE_AF_PP;
    gpio.GPIO_Pull      = GPIO_NO_PULL;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_FAST;
    gpio.GPIO_Current   = GPIO_HS_IO_DC_8mA;
    gpio.GPIO_Alternate = GPIO_AF1;
    GPIO_InitPeripheral(GPIOC, &gpio);

    gpio.Pin            = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
    gpio.GPIO_Pull      = GPIO_PULL_UP;
    gpio.GPIO_Current   = GPIO_DC_4mA;
    GPIO_InitPeripheral(GPIOC, &gpio);

    /* PC8 = D0 is AF0 on this board, not AF1 like the other data lines. */
    gpio.Pin            = GPIO_PIN_8;
    gpio.GPIO_Current   = GPIO_5VTOL_DC_2mA;
    gpio.GPIO_Alternate = GPIO_AF0;
    GPIO_InitPeripheral(GPIOC, &gpio);

    /* PD2 = SDMMC1.CMD, AF2. */
    gpio.Pin            = GPIO_PIN_2;
    gpio.GPIO_Current   = GPIO_HS_IO_DC_4mA;
    gpio.GPIO_Alternate = GPIO_AF2;
    GPIO_InitPeripheral(GPIOD, &gpio);
}

bool SD_App_Init(void)
{
    uint32_t timeout_start;
    uint32_t timeout_value;

    printf("\r\n===== N32H787 SDMMC1 bring-up =====\r\n");

    /* ---------------------------------------------------------------- clock */
    /* NTFx leaves SDMMC1 on AXIDIV/8 = 37.5 MHz, which the prescaler helper
     * above cannot divide exactly for either 400 kHz or 25 MHz. PLL2 is unused
     * everywhere else in this project, and HSE (25 MHz) is already required for
     * USB's reference clock, so moving SDMMC1 onto PLL2A = 500/5 = 100 MHz is
     * free and matches the validated example.
     *
     * Only ever call RCC_ConfigPll2 with ENABLE: it writes the PLL3 LDO
     * register (PLL3CTRL1), so the DISABLE path would disturb PLL3 and
     * therefore SDRAM. */
    RCC_ConfigHse(RCC_HSE_ENABLE);
    if (RCC_WaitHseStable() != SUCCESS)
    {
        printf("FAIL: HSE (%u Hz) did not start, cannot derive the SDMMC1 clock\r\n",
               (unsigned)HSE_VALUE);
        return false;
    }

    /* RCC_ConfigPll2() spins on the PLL2 lock bit with no timeout of its own.
     * Announce it first, so a hang here is distinguishable from a hang later. */
    printf("Starting PLL2 (HSE x 20 = 500 MHz) for the SDMMC1 kernel clock...\r\n");
    RCC_ConfigPll2(RCC_PLL_SRC_HSE, HSE_VALUE, 500000000U, ENABLE);
    RCC_ConfigPLL2ADivider(RCC_PLLA_DIV5);
    RCC_ConfigSDMMC1KerClk(RCC_SDMMC1KERCLK_SRC_PLL2A, RCC_SDMMC1KERCLK_AXIDIV1);

    /* NTFx only enables RCC_AXI_PERIPHEN_M7_SDMMC1; the example also needs the
     * low-power gate. */
    RCC_EnableAXIPeriphClk1(RCC_AXI_PERIPHEN_M7_SDMMC1 | RCC_AXI_PERIPHEN_M7_SDMMC1LP, ENABLE);

    /* Card power rail. The host wrapper registers themselves were already
     * programmed by SDMMC_Configuration() in n32h7xx_cfg.c, and changing the
     * kernel clock mux does not clear them, so they are not reprogrammed here.
     * (The example pairs its clock switch with a peripheral reset; that would
     * wipe the wrapper config and require re-running SDMMC_WrapperConfig.) */
    PWR_MoudlePowerEnable(HSC1_SDMMC1_PWRCTRL, ENABLE);
    printf("SDMMC1 kernel clock : %u Hz (PLL2A)\r\n", (unsigned)SD_APP_XIN_CLK);

    /* Redo the pin configuration n32h7xx_cfg.c left at default speed settings. */
    SD_App_ConfigurePads();

    /* --------------------------------------------------------------- card */
    g_sd_card.SDHOSTx = SDHOST1;
    g_sd_card.SDMMCx  = SDMMC1;
    g_sd_card.card_workmode.busClock_Hz          = SD_APP_BUS_CLK_HZ;
    g_sd_card.card_workmode.busWidth             = SDMMC_BusWdith4Bit;
    g_sd_card.card_workmode.mode                 = SDMMC_DS;
    /* Internal SDMA, not the CPU-polled data port. The polling write path
     * hands the host core a whole block (128 words) as fast as the CPU can
     * issue stores - about a microsecond, against the 41 us the card needs to
     * drain those bytes at 25 MHz - and on this core that burst comes out
     * mangled: a round-tripped block came back with a stale first word, some
     * source words repeated and the tail missing, differently on each attempt.
     * With SDMA the core fetches the block from memory itself, so the CPU
     * timing drops out of the data path entirely. */
    g_sd_card.card_workmode.dma                  = SDMMC_SDMA;
    g_sd_card.card_workmode.operationVoltageflag = SD_OperationVoltage330V;
    SDMMC_TModeStructInit(&g_sd_card.TMODE_truct);

    SDMMC_EnableFlagStatus(g_sd_card.SDHOSTx, SDHOST_AllInterruptFlags, ENABLE);
    SDMMC_ConfigCardDetectSignal(g_sd_card.SDHOSTx, SDMMC_CARDDETECT_TEST, SDMMC_CARDTESTLEVEL_HIGH);

    /* Wait up to 1 s for the card to be reported present. */
    SD_SysTick_start_time();
    timeout_start = SD_systick_timeoutms;
    timeout_value = 0;
    while (SDMMC_GetPresentFlagStatus(g_sd_card.SDHOSTx, SDHOST_CardInsertedFlag) != SET)
    {
        timeout_value = SD_User_Time_Read(timeout_start);
        if (timeout_value >= SD_APP_CARD_TIMEOUT_MS)
        {
            printf("FAIL: no card detected after %u ms\r\n", (unsigned)SD_APP_CARD_TIMEOUT_MS);
            return false;
        }
    }
    printf("Card detected\r\n");

    SDMMC_EnablePower(g_sd_card.SDHOSTx, ENABLE);

    /* Identification clock: 400 kHz. */
    SDMMC_SetSdClock(g_sd_card.SDHOSTx, DISABLE, SD_App_Prescaler(400000U, SD_APP_BUS_CLK_HZ));

    /* ------------------------------------------------------------ CMD0/CMD8 */
    if (SD_NormalCMD_Send(&g_sd_card, SDMMC_GoIdleState, 0x00U, CARD_ResponseTypeNone) != Status_Success)
    {
        printf("FAIL: CMD0\r\n");
        return false;
    }

    /* arg[19:16] = 0001b for 2.7..3.6 V, arg[15:8] = 0xAA check pattern. */
    if (SD_NormalCMD_Send(&g_sd_card, SD_SendInterfaceCondition, 0x1AAU, CARD_ResponseTypeR7) != Status_Success)
    {
        printf("FAIL: CMD8\r\n");
        return false;
    }
    if ((g_sd_card.command.response[0] & 0xFFU) != 0xAAU)
    {
        printf("FAIL: CMD8 echo mismatch (0x%02X), not an SD 2.0+ card\r\n",
               (unsigned)(g_sd_card.command.response[0] & 0xFFU));
        return false;
    }
    g_sd_card.sd_card_information.flags |= SD_SupportSdhcFlag;
    printf("CMD8  OK, SD spec 2.00+\r\n");

    /* ------------------------------------------------------------- ACMD41 */
    /* arg[31] busy, arg[30] HCS (host supports SDHC/SDXC), arg[23:15] VDD
     * window. 0x40100000 = HCS set, 3.2..3.3 V, no 1.8 V request. */
    g_sd_card.sd_card_information.busy = 0x00U;
    SD_SysTick_start_time();
    timeout_start = SD_systick_timeoutms;
    timeout_value = 0;
    while ((g_sd_card.sd_card_information.busy != 0x80000000U) && (timeout_value < SD_APP_CARD_TIMEOUT_MS))
    {
        if (SD_AutoCMD_Send(&g_sd_card, SD_ApplicationSendOperationCondition, 0x40100000U,
                            CARD_ResponseTypeR3) != Status_Success)
        {
            /* Not fatal on its own: a card that is still waking up NAKs the
             * first few, and the loop retries until the timeout. */
            printf("  ACMD41 retry (status error)\r\n");
        }
        g_sd_card.sd_card_information.busy = (g_sd_card.command.response[0] & 0x80000000U);
        timeout_value = SD_User_Time_Read(timeout_start);
    }

    if (timeout_value >= SD_APP_CARD_TIMEOUT_MS)
    {
        printf("FAIL: ACMD41 timed out after %u ms\r\n", (unsigned)SD_APP_CARD_TIMEOUT_MS);
        return false;
    }

    if ((g_sd_card.command.response[0] & 0x40000000U) != 0U)
    {
        g_sd_card.sd_card_information.flags |= SD_SupportHighCapacityFlag;
    }
    if ((g_sd_card.command.response[0] & 0x01000000U) != 0U)
    {
        g_sd_card.sd_card_information.flags |= SD_SupportVoltage180v;
    }
    g_sd_card.sd_card_information.ocr = (g_sd_card.command.response[0] & 0x00FFFF00U) >> 8U;
    printf("ACMD41 OK, OCR = 0x%08X, %s\r\n", (unsigned)g_sd_card.sd_card_information.ocr,
           ((g_sd_card.sd_card_information.flags & SD_SupportHighCapacityFlag) != 0U)
               ? "high capacity (block addressing)"
               : "standard capacity (byte addressing)");

    /* -------------------------------------------------- CMD2 / CMD3 / CMD9 */
    if (SD_NormalCMD_Send(&g_sd_card, SDMMC_AllSendCid, 0x00U, CARD_ResponseTypeR2) != Status_Success)
    {
        printf("FAIL: CMD2\r\n");
        return false;
    }
    SD_DecodeCid(&g_sd_card);

    if (SD_NormalCMD_Send(&g_sd_card, SD_SendRelativeAddress, 0x00U, CARD_ResponseTypeR6) != Status_Success)
    {
        printf("FAIL: CMD3\r\n");
        return false;
    }
    g_sd_card.sd_card_information.rca = (g_sd_card.command.response[0] & 0xFFFF0000U) >> 16U;

    if (SD_NormalCMD_Send(&g_sd_card, SDMMC_SendCsd, g_sd_card.sd_card_information.rca << 16U,
                          CARD_ResponseTypeR2) != Status_Success)
    {
        printf("FAIL: CMD9\r\n");
        return false;
    }
    SD_DecodeCsd(&g_sd_card);

    /* ---------------------------------------------------------- CMD7 / SCR */
    if (SD_NormalCMD_Send(&g_sd_card, SDMMC_SelectCard, g_sd_card.sd_card_information.rca << 16U,
                          CARD_ResponseTypeR1b) != Status_Success)
    {
        printf("FAIL: CMD7\r\n");
        return false;
    }

    if (SD_SendSCR(&g_sd_card) != Status_Success)
    {
        printf("FAIL: ACMD51\r\n");
        return false;
    }

    if (SD_PollingCardStatusBusy(&g_sd_card, SD_CARD_ACCESS_WAIT_IDLE_TIMEOUT) != Status_CardStatusIdle)
    {
        printf("FAIL: card never went idle (CMD13)\r\n");
        return false;
    }

    /* ------------------------------------------------------- CMD16 / 4-bit */
    if (SD_NormalCMD_Send(&g_sd_card, SDMMC_SetBlockLength, FSL_SDMMC_DEFAULT_BLOCK_SIZE,
                          CARD_ResponseTypeR1) != Status_Success)
    {
        printf("FAIL: CMD16\r\n");
        return false;
    }

    if ((g_sd_card.sd_card_information.flags & SD_Support4BitWidthFlag) == 0U)
    {
        printf("FAIL: card does not report 4-bit bus support (SCR)\r\n");
        return false;
    }
    /* ACMD6: 00b = 1-bit, 10b = 4-bit. */
    if (SD_AutoCMD_Send(&g_sd_card, SD_ApplicationSetBusWdith, 0x00000002U, CARD_ResponseTypeR1) != Status_Success)
    {
        printf("FAIL: ACMD6\r\n");
        return false;
    }
    SDMMC_ConfigBusWidth(g_sd_card.SDHOSTx, SDHOST_DataBusWidth4Bit);
    printf("ACMD6 OK, 4-bit bus\r\n");

    /* ------------------------------------------------------------ CMD6 */
    /* Switching the bus timing is an optimisation, not a prerequisite: a card
     * without CMD6 still reads fine at default speed, which is what
     * card_workmode.mode already asks for. Warn instead of failing. */
    if (SD_SelectBusTiming(&g_sd_card) != Status_Success)
    {
        printf("WARN: CMD6 not supported by this card, staying at default speed\r\n");
    }
    SDMMC_ConfigWorkMode(g_sd_card.SDHOSTx, g_sd_card.card_workmode.mode);

    /* ------------------------------------------------- final bus clock */
    SDMMC_SetSdClock(g_sd_card.SDHOSTx, DISABLE,
                     SD_App_Prescaler(SD_APP_BUS_CLK_HZ, SD_APP_BUS_CLK_HZ));
    printf("Bus clock set to %u Hz\r\n", (unsigned)SD_APP_BUS_CLK_HZ);

    printf("===== SD card ready =====\r\n");
    return true;
}

sd_card_t *SD_App_Card(void)
{
    return &g_sd_card;
}

uint32_t SD_App_ConfigureBus(SD_ModeConfig mode, uint32_t hz)
{
    uint32_t prescaler = SD_App_Prescaler(hz, SD_APP_BUS_CLK_HZ);

    if (prescaler == 0U)
    {
        return 0U;
    }

    /* Order matters: CMD6 only works while the bus is still running at default
     * speed, so switch the timing first and raise the clock afterwards. */
    g_sd_card.card_workmode.mode = mode;
    if (SD_SelectBusTiming(&g_sd_card) != Status_Success)
    {
        g_sd_card.card_workmode.mode = SDMMC_DS;
        return 0U;
    }
    SDMMC_ConfigWorkMode(g_sd_card.SDHOSTx, mode);
    SDMMC_SetSdClock(g_sd_card.SDHOSTx, DISABLE, prescaler);

    /* The controller divides the kernel clock by 2 * prescaler. */
    return SD_APP_XIN_CLK / (2U * prescaler);
}

/* -------------------------------------------------------------------------- */
/* Report and read test                                                       */
/* -------------------------------------------------------------------------- */

static void SD_App_PrintCard(void)
{
    const sd_card_info *info = &g_sd_card.sd_card_information;
    char name[SD_PRODUCT_NAME_BYTES + 1];
    uint32_t i;

    for (i = 0; i < SD_PRODUCT_NAME_BYTES; i++)
    {
        name[i] = (char)info->cid.productName[i];
    }
    name[SD_PRODUCT_NAME_BYTES] = '\0';

    printf("CID  : manufacturer 0x%02X, OEM 0x%04X, name '%s', rev %u.%u, serial 0x%08X\r\n",
           (unsigned)info->cid.manufacturerID, (unsigned)info->cid.applicationID, name,
           (unsigned)(info->cid.productVersion >> 4), (unsigned)(info->cid.productVersion & 0x0FU),
           (unsigned)info->cid.productSerialNumber);
    printf("       mfg date %u/%u\r\n", (unsigned)((info->cid.manufacturerData >> 4) + 2000U),
           (unsigned)(info->cid.manufacturerData & 0x0FU));

    printf("CSD  : structure v%u, transfer speed 0x%02X, read block length %u\r\n",
           (unsigned)info->csd.csdStructure, (unsigned)info->csd.transferSpeed,
           (unsigned)info->csd.readBlockLength);
    printf("       device size 0x%08X\r\n", (unsigned)info->csd.deviceSize);

    printf("RCA  : %u\r\n", (unsigned)info->rca);
    printf("Size : %u blocks x %u B = %u MiB\r\n", (unsigned)info->blockCount,
           (unsigned)info->blockSize, (unsigned)(info->blockCount / 2048U));

    printf("Type : %s%s%s%s\r\n",
           ((info->flags & SD_SupportSdhcFlag) != 0U) ? "SDHC " : "",
           ((info->flags & SD_SupportSdxcFlag) != 0U) ? "SDXC " : "",
           ((info->flags & SD_SupportHighCapacityFlag) != 0U) ? "high-capacity " : "",
           ((info->flags & SD_Support4BitWidthFlag) != 0U) ? "4-bit" : "");
}

void SD_App_Test(void)
{
    uint32_t i;
    uint32_t sum_a = 0U;
    uint32_t sum_b = 0U;
    Status_card status;

    SD_App_PrintCard();

    /* Read the same 4 KiB twice and compare. Independent of what happens to be
     * on the card, so a mismatch means the read path is wrong. Read-only: no
     * writes are issued. */
    printf("Read : blocks 0..%u into two buffers...\r\n", (unsigned)(SD_APP_TEST_BLOCKS - 1U));

    status = SD_ReadBlocks(&g_sd_card, g_sd_read_a, 0U, SD_APP_TEST_BLOCKS);
    if (status != Status_Success)
    {
        printf("FAIL: read #1, status %d\r\n", (int)status);
        return;
    }

    status = SD_ReadBlocks(&g_sd_card, g_sd_read_b, 0U, SD_APP_TEST_BLOCKS);
    if (status != Status_Success)
    {
        printf("FAIL: read #2, status %d\r\n", (int)status);
        return;
    }

    for (i = 0; i < SD_APP_TEST_WORDS; i++)
    {
        sum_a += g_sd_read_a[i];
        sum_b += g_sd_read_b[i];
    }

    printf("       first 16 bytes:");
    for (i = 0; i < 4U; i++)
    {
        printf(" %08X", (unsigned)g_sd_read_a[i]);
    }
    printf("\r\n");
    printf("       checksum #1 0x%08X, #2 0x%08X\r\n", (unsigned)sum_a, (unsigned)sum_b);

    if (sum_a != sum_b)
    {
        printf("FAIL: the two reads differ - read path is not reliable\r\n");
        return;
    }

    printf("PASS: %u blocks read back identically twice\r\n", (unsigned)SD_APP_TEST_BLOCKS);
}

void SD_App_Run(void)
{
    SD_App_DebugInit();

    if (SD_App_Init())
    {
        SD_App_Test();

        /* Kept behind a single main.c hook on purpose: main.c is an NTFx
         * generated file, so every extra line in it is a line to re-add by
         * hand the next time the project is regenerated. */
        SD_Bench_Run();
    }
    else
    {
        printf("SD bring-up failed, no read test performed\r\n");
    }
}
