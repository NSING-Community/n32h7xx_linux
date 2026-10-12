/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sdram_app.c
 * @brief SDRAM bring-up for the N32H787 HMI V1.1 board (CM7).
 *
 * U11 is an IS42S32800J-6BLI: 32 MiB, 4 banks x 4096 rows x 512 columns, x32,
 * CAS 3, burst 2, 133 MHz. All 57 of its pins sit on AF0.
 *
 * This is a port of the workbench that was validated on this board in the
 * h760_tf tree - USER/src/board_sdram.c for the sequence, BENCH/sdram.c for
 * the tests - carrying the same pin list and the same register values rather
 * than re-deriving them.
 *
 * N32Cube generates its own SDRAM_Configuration() and this project calls it,
 * but it cannot be used as-is here:
 *
 *   - it muxes seven of the signals to pads this board does not route them on:
 *     D8/D9/D12 to PA4/PA5/PC0, DQM1 to PG2, CKE0 to PC3, NCAS to PC5, NWE to
 *     PH5, where the board has PE11/PE12/PE15, PE1, PH2, PG15, PC0. PC0 and
 *     PG2 this module re-muxes as the board needs them, but PA4, PA5, PH5 and
 *     PC3 it leaves behind, pointing a controller signal at a pad with nothing
 *     on it - which the self-test below does not tolerate, so
 *     SDRAM_App_Config() hands them back. It also never puts the pads into
 *     high speed mode, nor enables the VREF/DSN/DSP remaps that the workbench
 *     switches on for every pin;
 *   - it sizes the device as 4 banks x 4096 rows x 256 columns - 16 MiB - and
 *     masks the address window at 16 MiB to match, so half of U11 is
 *     unreachable and the other half aliases;
 *   - it clocks the memory at PLL3A/4 where the workbench uses PLL3A;
 *   - it uses the EVB timing set {7,9,3,4,3,9,4} and a 1000-cycle refresh
 *     interval, both of which belong to a different part;
 *   - it turns the refresh command on *before* the initialisation sequence
 *     instead of after it, issues 2 refresh commands where the part needs 8,
 *     and never resets the controller first.
 *
 * Rather than patch generated code, this module lets it run and then redoes
 * the whole thing from SDRAM_DeInit() up; the generated call is left where it
 * is so that regenerating the project stays a no-op. main() calls
 * SDRAM_App_Config() outside the NTFx block.
 */
#include "sdram_app.h"

#include <stdio.h>

#include "n32h7xx.h"
#include "n32h7xx_rcc.h"
#include "n32h7xx_gpio.h"
#include "n32h7xx_sdram.h"

/** Memory clock the register values below were tuned for. */
#if defined(BOOT_SDRAM_READ_TEST)
static uint32_t g_sdram_test_cas = 3U;
#if SDRAM_TEST_PLL_DIV == 5
#define SDRAM_TEST_PLL_SETTING RCC_PLLA_DIV5
#elif SDRAM_TEST_PLL_DIV == 10
#define SDRAM_TEST_PLL_SETTING RCC_PLLA_DIV10
#elif SDRAM_TEST_PLL_DIV == 20
#define SDRAM_TEST_PLL_SETTING RCC_PLLA_DIV20
#else
#error Unsupported SDRAM diagnostic divider
#endif
#define SDRAM_HZ (665000000U / SDRAM_TEST_PLL_DIV)
#if SDRAM_TEST_BURST == 2
#define SDRAM_TEST_BURST_SETTING SDRAM_DEVICE_BURSTLEN_2
#define SDRAM_TEST_MODE 0x31U
#elif SDRAM_TEST_BURST == 4
#define SDRAM_TEST_BURST_SETTING SDRAM_DEVICE_BURSTLEN_4
#define SDRAM_TEST_MODE 0x32U
#elif SDRAM_TEST_BURST == 8
#define SDRAM_TEST_BURST_SETTING SDRAM_DEVICE_BURSTLEN_8
#define SDRAM_TEST_MODE 0x33U
#else
#error Unsupported diagnostic SDRAM burst length
#endif
#else
#ifndef SDRAM_BOOT_PLL_DIV
#define SDRAM_BOOT_PLL_DIV 5
#endif
#if SDRAM_BOOT_PLL_DIV == 5
#define SDRAM_BOOT_PLL_SETTING RCC_PLLA_DIV5
#elif SDRAM_BOOT_PLL_DIV == 10
#define SDRAM_BOOT_PLL_SETTING RCC_PLLA_DIV10
#elif SDRAM_BOOT_PLL_DIV == 20
#define SDRAM_BOOT_PLL_SETTING RCC_PLLA_DIV20
#else
#error Unsupported boot SDRAM divider
#endif
#define SDRAM_HZ (665000000U / SDRAM_BOOT_PLL_DIV)
#endif

/**
 * Refresh interval in memory clock cycles: 64 ms / 4096 rows / 7.52 ns =
 * 2078 cycles is the datasheet limit, the vendor's HMI value of 1872 keeps
 * margin under it and is the one the workbench ran with.
 */
#if defined(BOOT_SDRAM_READ_TEST)
#define SDRAM_REFRESH_CYCLES (1872U * 5U / SDRAM_TEST_PLL_DIV)
#else
#define SDRAM_REFRESH_CYCLES (1872U * 5U / SDRAM_BOOT_PLL_DIV)
#endif

/** How long the self-test leaves the patterns in the device before re-reading. */
#define SDRAM_RETENTION_MS (250U)

/** Words in the device, and in one bank. */
#define SDRAM_WORDS     (SDRAM_APP_BYTES / 4U)
#define SDRAM_BANK_WORDS (SDRAM_WORDS / 4U)

/**
 * The board's SDRAM pins, all AF0. The high speed pin index is used for the
 * VREF/DSN/DSP remaps, which have to be on for the same pad.
 */
#define SD_PIN(port, pin) {GPIO##port, GPIO_PIN_##pin, AFIO_HIGH_SPEED_IO_P##port##pin}
static const struct
{
    GPIO_Module *port;
    uint32_t pin;
    AFIO_HIGH_SPEED_PIN hs;
} g_sdram_pins[] = {
    SD_PIN(F, 0), SD_PIN(F, 1), SD_PIN(F, 2), SD_PIN(F, 3),
    SD_PIN(F, 4), SD_PIN(F, 5), SD_PIN(F, 12), SD_PIN(F, 13),
    SD_PIN(F, 14), SD_PIN(F, 15), SD_PIN(G, 0), SD_PIN(G, 1), SD_PIN(G, 2),
    SD_PIN(G, 4), SD_PIN(G, 5),                          /* BA0, BA1 */
    SD_PIN(D, 14), SD_PIN(D, 15), SD_PIN(D, 0), SD_PIN(D, 1),
    SD_PIN(E, 7), SD_PIN(E, 8), SD_PIN(E, 9), SD_PIN(E, 10),
    SD_PIN(E, 11), SD_PIN(E, 12), SD_PIN(E, 13), SD_PIN(E, 14), SD_PIN(E, 15),
    SD_PIN(D, 8), SD_PIN(D, 9), SD_PIN(D, 10),           /* D13-D15 */
    SD_PIN(H, 8), SD_PIN(H, 9), SD_PIN(H, 10), SD_PIN(H, 11),
    SD_PIN(H, 12), SD_PIN(H, 13), SD_PIN(H, 14), SD_PIN(H, 15),
    SD_PIN(I, 0), SD_PIN(I, 1), SD_PIN(I, 2), SD_PIN(I, 3),
    SD_PIN(I, 6), SD_PIN(I, 7), SD_PIN(I, 9), SD_PIN(I, 10),  /* D28-D31 */
    SD_PIN(E, 0), SD_PIN(E, 1), SD_PIN(I, 4), SD_PIN(I, 5),   /* DQM0-DQM3 */
    SD_PIN(C, 2), SD_PIN(C, 0), SD_PIN(F, 11), SD_PIN(G, 15), /* CS, WE, RAS, CAS */
    SD_PIN(H, 2), SD_PIN(G, 8)                                /* CKE, CLK */
};

static uint32_t g_sdram_status = SDRAM_APP_STATUS_UNUSED;
static uint32_t g_sdram_core_hz;

volatile uint32_t g_sdram_error_address;
volatile uint32_t g_sdram_error_expected;
volatile uint32_t g_sdram_error_actual;

uint32_t SDRAM_App_Status(void)
{
    return g_sdram_status;
}

/**
 * @brief Spin for @p us microseconds.
 *
 * DWT rather than the 1 ms tick: the initialisation sequence needs 1 ms twice
 * and the tick is enough for that, but the retention wait and anything later
 * wants finer resolution. TRCENA is re-armed inside the loop so a detaching
 * debugger cannot leave it spinning for ever.
 */
static void Sdram_DelayUs(uint32_t us)
{
    uint32_t cycles = (g_sdram_core_hz / 1000000U) * us;
    uint32_t start;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();

    start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/**
 * @brief Put one command from the initialisation sequence on the pins.
 *
 * Writing the operation setup register only latches it; the controller sends
 * it on the next access to the SDRAM region, which is what reading OR does.
 */
static void Sdram_Command(uint32_t opcode, uint32_t address)
{
    SDRAM_EnableClock(ENABLE);
    SDRAM_SetOperationCode(opcode);
    SDRAM_SetDeviceSelect(SDRAM_CS_SDRAM1_ONLY);
    SDRAM_SetBank(SDRAM_BANKADD_1);
    SDRAM_SetAddress(address);
    (void)SDRAM->OR;
    __DSB();
}

#if defined(BOOT_SDRAM_READ_TEST)
/* Diagnostic-only MR change; code/stack remain in internal memory. */
void SDRAM_TestSetBurst(uint32_t burst)
{
    uint32_t setting = burst == 2U ? SDRAM_DEVICE_BURSTLEN_2 :
                       burst == 4U ? SDRAM_DEVICE_BURSTLEN_4 : SDRAM_DEVICE_BURSTLEN_8;
    uint32_t mode = (g_sdram_test_cas << 4) | (burst == 2U ? 1U : burst == 4U ? 2U : 3U);
    __DSB(); SDRAM->CBO = 0U; (void)SDRAM->CBO; __DSB();
    SDRAM_EnableRefreshCMD(SDRAM_DEVICE_1, DISABLE);
    Sdram_DelayUs(10U);
    Sdram_Command(SDRAM_OPCODE_PRECHRG, 0x400U); /* A10: all banks */
    Sdram_DelayUs(10U);
    SDRAM_ConfigBurstLength(SDRAM_DEVICE_1, setting);
    SDRAM_ConfigCASLatency(SDRAM_DEVICE_1, g_sdram_test_cas == 2U ?
                           SDRAM_DEVICE_CASLTCY_2 : SDRAM_DEVICE_CASLTCY_3);
    Sdram_Command(SDRAM_OPCODE_LOADMODE, mode);
    Sdram_DelayUs(10U);
    Sdram_Command(SDRAM_OPCODE_NONE, 0U);
    SDRAM_EnableRefreshCMD(SDRAM_DEVICE_1, ENABLE);
    (void)SDRAM->CBO; __DSB();
}

void SDRAM_TestSetCAS(uint32_t cas)
{
    if (cas == 2U && SDRAM_HZ > 100000000U) {
        printf("FAIL CAS2 exceeds part's 100 MHz limit\r\n");
        return;
    }
    g_sdram_test_cas = cas;
    SDRAM_TestSetBurst(2U);
}
#endif

/**
 * @brief Read one word back and compare it.
 *
 * The controller has write and read buffers of its own, so a read straight
 * after a write can be answered from the buffer and pass whatever the memory
 * actually holds. CBO drains the write buffer and invalidates the read one.
 */
static bool Sdram_CheckWord(volatile uint32_t *p, uint32_t expected)
{
    uint32_t actual;

    __DSB();
    SDRAM->CBO = 0U;
    (void)SDRAM->CBO;
    __DSB();

    actual = *p;
    if (actual == expected)
    {
        return true;
    }

    g_sdram_error_address  = (uint32_t)p;
    g_sdram_error_expected = expected;
    g_sdram_error_actual   = actual;
    return false;
}

/**
 * @brief Walking 1 and walking 0 on the data bus, at every bank base.
 *
 * This is the test that names a bad bit: a data line on the wrong pad, or not
 * connected, shows up as one bit that never moves, and doing it at the four
 * bank bases catches a line that is only broken on one bank's routing.
 */
static bool Sdram_TestDataBus(void)
{
    volatile uint32_t *p = (volatile uint32_t *)SDRAM_APP_BASE;
    uint32_t bank;
    uint32_t bit;

    for (bank = 0U; bank < 4U; bank++)
    {
        uint32_t base = bank * SDRAM_BANK_WORDS;

        for (bit = 0U; bit < 32U; bit++)
        {
            p[base] = (uint32_t)1U << bit;
            if (!Sdram_CheckWord(p + base, (uint32_t)1U << bit))
            {
                return false;
            }

            p[base] = ~((uint32_t)1U << bit);
            if (!Sdram_CheckWord(p + base, ~((uint32_t)1U << bit)))
            {
                return false;
            }
        }

        /* Then one byte at a time, so each DQM has to mask the three lanes it
         * is not writing. A lane that is never masked takes the other lanes'
         * data with it, which walking bits alone can miss. */
        {
            volatile uint8_t *bytes = (volatile uint8_t *)&p[base];
            uint32_t expected = 0U;
            unsigned lane;

            p[base] = 0U;
            for (lane = 0U; lane < 4U; lane++)
            {
                bytes[lane] = (uint8_t)(0x59U + lane);
                expected |= ((uint32_t)(0x59U + lane)) << (lane * 8U);
                if (!Sdram_CheckWord(p + base, expected))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

/**
 * @brief Sparse write across the whole device, one address line at a time.
 *
 * Every power-of-two word offset gets a different pattern, then each one is
 * flipped and all the others are re-read: a shorted or aliased address line
 * makes two cells the same cell, and the test then sees one of them change
 * when the other was written.
 */
static bool Sdram_TestAddressLines(void)
{
    volatile uint32_t *p = (volatile uint32_t *)SDRAM_APP_BASE;
    uint32_t test;

    p[0] = 0x55555555U;
    for (test = 1U; test < SDRAM_WORDS; test <<= 1)
    {
        p[test] = 0xAAAAAAAAU;
    }

    for (test = 1U; test < SDRAM_WORDS; test <<= 1)
    {
        uint32_t bit;

        p[test] = 0x55555555U;
        if (!Sdram_CheckWord(p + test, 0x55555555U))
        {
            return false;
        }

        for (bit = 1U; bit < SDRAM_WORDS; bit <<= 1)
        {
            if (!Sdram_CheckWord(p + bit, (bit == test) ? 0x55555555U : 0xAAAAAAAAU))
            {
                return false;
            }
        }

        p[test] = 0xAAAAAAAAU;
    }

    return true;
}

/**
 * @brief 4 KiB of pseudo-random data at each bank base, once as-is and once
 *        inverted, then the same data re-read after a retention wait.
 *
 * Walking bits prove the lines are connected; this proves the cells hold what
 * was put in them, including while the refresh counter is running - a refresh
 * interval set too long for the part is exactly what a retention failure means.
 */
static bool Sdram_TestBanks(void)
{
    volatile uint32_t *p = (volatile uint32_t *)SDRAM_APP_BASE;
    uint32_t bank;
    unsigned pass;
    uint32_t i;

    for (bank = 0U; bank < 4U; bank++)
    {
        uint32_t base = bank * SDRAM_BANK_WORDS;

        for (pass = 0U; pass < 2U; pass++)
        {
            for (i = 0U; i < 1024U; i++)
            {
                uint32_t value = 0x5AA53CC3U ^ ((base + i) * 0x9E3779B9U);
                p[base + i] = (pass != 0U) ? ~value : value;
            }

            __DSB();
            for (i = 0U; i < 1024U; i++)
            {
                uint32_t value = 0x5AA53CC3U ^ ((base + i) * 0x9E3779B9U);
                if (!Sdram_CheckWord(p + base + i, (pass != 0U) ? ~value : value))
                {
                    return false;
                }
            }
        }
    }

    /* Re-read the last pass after leaving it alone for a while. */
    Sdram_DelayUs(SDRAM_RETENTION_MS * 1000U);
    for (bank = 0U; bank < 4U; bank++)
    {
        uint32_t base = bank * SDRAM_BANK_WORDS;

        for (i = 0U; i < 1024U; i++)
        {
            uint32_t value = 0x5AA53CC3U ^ ((base + i) * 0x9E3779B9U);
            if (!Sdram_CheckWord(p + base + i, ~value))
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief Read one word after draining the controller's buffers.
 *
 * Used to say something about a word that failed a check: reading it a second
 * time separates a cell that never took the write from one that came back
 * wrong once and reads correctly now.
 */
static uint32_t Sdram_ReadWord(volatile uint32_t *p)
{
    __DSB();
    SDRAM->CBO = 0U;
    (void)SDRAM->CBO;
    __DSB();

    return *p;
}

static const char *Sdram_StageName(unsigned stage)
{
    switch (stage)
    {
    case 1U: return "data bus";
    case 2U: return "address lines";
    default: return "banks and retention";
    }
}

static bool Sdram_SelfTest(void)
{
    unsigned stage;

    for (stage = 1U; stage <= 3U; stage++)
    {
        bool ok;

        if (stage == 1U)
        {
            ok = Sdram_TestDataBus();
        }
        else if (stage == 2U)
        {
            ok = Sdram_TestAddressLines();
        }
        else
        {
            ok = Sdram_TestBanks();
        }

        if (!ok)
        {
            volatile uint32_t *bad = (volatile uint32_t *)g_sdram_error_address;

            printf("FAIL: SDRAM %s: read %08X at %08X, expected %08X\r\n",
                   Sdram_StageName(stage),
                   (unsigned)g_sdram_error_actual,
                   (unsigned)g_sdram_error_address,
                   (unsigned)g_sdram_error_expected);

            if (stage == 2U)
            {
                /* Sequenced by hand: printf may evaluate its arguments in any
                 * order, and each of these reads drains the buffers first. */
                uint32_t before = Sdram_ReadWord(bad - 1U);
                uint32_t again  = Sdram_ReadWord(bad);
                uint32_t after  = Sdram_ReadWord(bad + 1U);

                printf("      re-read %08X %08X %08X\r\n",
                       (unsigned)before, (unsigned)again, (unsigned)after);
            }

            return false;
        }

        printf("SDRAM %s PASS\r\n", Sdram_StageName(stage));
    }

    return true;
}

bool SDRAM_App_Config(void)
{
    RCC_ClocksTypeDef clocks;
    GPIO_InitType gpio;
    SDRAM_TimingType timing;
    unsigned i;

    if (g_sdram_status == SDRAM_APP_STATUS_READY)
    {
        return true;
    }

    printf("\r\n===== SDRAM bring-up =====" "\r\n"
           "Device : IS42S32800J-6BLI, 32 MiB, 4 banks x 4096 rows x 512 cols" "\r\n");

    /* The M7's default attributes for 0xC0000000 are Device, which is correct
     * for the controller and needs no help, but a cache in front of it would
     * need maintenance around every transfer that the CPU is not doing. This
     * project runs with no cache at all, and the region stays Device: the
     * bootloader's DMA into SDRAM is then cache-clean by construction. */
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U)
    {
        printf("FAIL: D-cache is on, refusing to touch SDRAM before setting up"
               " its attributes\r\n");
        g_sdram_status = SDRAM_APP_STATUS_UNSAFE;
        return false;
    }

    RCC_GetClocksFreqValue(&clocks);
    g_sdram_core_hz = clocks.M7ClkFreq;

    /* The workbench drives PLL3 from the 25 MHz crystal: HSE x 26.6 =
     * 665 MHz, PLL3A = /5 = 133 MHz.
     *
     * NTFx instead derives PLL3 from HSI, the internal RC. Its register values
     * also work out to 133 MHz as far as the driver's own arithmetic is
     * concerned, but that figure is only as good as the RC oscillator behind
     * it, and what hangs off it here is not forgiving: the timing registers
     * below are nanoseconds, and the controller samples the device's data
     * through a fixed delay chain that was tuned against a crystal. This is a
     * precaution, not a fix for anything observed - the bit errors the self-
     * test used to report were the stray pads handled above, and they were
     * unaffected by the PLL3 source. Nothing else in the project consumes PLL3
     * (PLL3B and PLL3C are divided down and never selected) and the controller
     * is reprogrammed from scratch below, so putting the SDRAM clock on the
     * crystal costs nothing. */
    RCC_ConfigHse(RCC_HSE_ENABLE);
    if (RCC_WaitHseStable() != SUCCESS)
    {
        printf("FAIL: HSE did not start\r\n");
        g_sdram_status = SDRAM_APP_STATUS_CLOCK;
        return false;
    }

    if (RCC_ConfigPll3(RCC_PLL_SRC_HSE, HSE_VALUE,
                       (uint64_t)133000000U * 5U, ENABLE) != SUCCESS)
    {
        printf("FAIL: PLL3 did not lock\r\n");
        g_sdram_status = SDRAM_APP_STATUS_CLOCK;
        return false;
    }
#if defined(BOOT_SDRAM_READ_TEST)
    RCC_ConfigPLL3ADivider(SDRAM_TEST_PLL_SETTING);
#else
    RCC_ConfigPLL3ADivider(SDRAM_BOOT_PLL_SETTING);
#endif

    RCC_GetClocksFreqValue(&clocks);
    printf("Buses  : M7 %u Hz, PLL3A %u Hz (HSE)\r\n",
           (unsigned)clocks.M7ClkFreq, (unsigned)clocks.PLL3AClkFreq);

    if (clocks.PLL3AClkFreq != SDRAM_HZ)
    {
        printf("FAIL: PLL3A is %u Hz, wanted %u\r\n",
               (unsigned)clocks.PLL3AClkFreq, (unsigned)SDRAM_HZ);
        g_sdram_status = SDRAM_APP_STATUS_CLOCK;
        return false;
    }

    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOC | RCC_AHB5_PERIPHEN_M7_GPIOD |
                             RCC_AHB5_PERIPHEN_M7_GPIOE | RCC_AHB5_PERIPHEN_M7_GPIOF |
                             RCC_AHB5_PERIPHEN_M7_GPIOG | RCC_AHB5_PERIPHEN_M7_GPIOH, ENABLE);
    RCC_EnableAHB5PeriphClk2(RCC_AHB5_PERIPHEN_M7_GPIOI | RCC_AHB5_PERIPHEN_M7_AFIO, ENABLE);

    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode      = GPIO_MODE_AF_PP;
    gpio.GPIO_Alternate = GPIO_AF0;
    gpio.GPIO_Pull      = GPIO_NO_PULL;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_FAST;
    gpio.GPIO_Current   = GPIO_DC_2mA;

    for (i = 0U; i < (sizeof(g_sdram_pins) / sizeof(g_sdram_pins[0])); i++)
    {
        gpio.Pin = g_sdram_pins[i].pin;
        GPIO_InitPeripheral(g_sdram_pins[i].port, &gpio);

        /* High speed mode plus the three remaps: the SDRAM controller drives
         * these pads through the high speed path, which is a separate
         * connection from the ordinary AF0 one. */
        AFIO_ConfigHSMODE(g_sdram_pins[i].port, g_sdram_pins[i].pin, ENABLE);
        AFIO_ConfigHSMODEVREFRemap(g_sdram_pins[i].hs, ENABLE);
        AFIO_ConfigHSPortDSNRemap(g_sdram_pins[i].hs, ENABLE);
        AFIO_ConfigHSPortDSPRemap(g_sdram_pins[i].hs, ENABLE);
    }

    /* N32Cube's own SDRAM_Configuration() has already run by now, and it muxed
     * pads for a pinout that is not this board's - see the labels in
     * CM7/src/n32h7xx_cfg.c. Besides the pads the two share, it leaves PA4 and
     * PA5 on AF0 (its SDRAM.D8 and D9), PH5 on AF0 (its SDRAM.NWE) and PC3 on
     * AF0 (its SDRAM.CKE0); the board routes those signals to PE11, PE12 and
     * PC0 instead. So the controller drives and samples a D8, a D9, an NWE and
     * a CKE0 on pads that nothing connects them to, in parallel with the pads
     * that do carry them. Hand them back, the way the workbench this module
     * comes from had them - it owned the whole SDRAM pinout and never muxed
     * these at all.
     *
     * This is what the address-line test was tripping over: with PA4 and PA5
     * still muxed, it read bit 8 back high at 0xC0000008 - D8, the first data
     * line of byte lane 1, which is exactly the signal the generator puts on
     * PA4. Expect the data pads to be the ones that matter; a duplicated NWE
     * or CKE0 is an output with nothing listening, which is also why PC3 is
     * released for tidiness rather than out of evidence, and why PC5 is left
     * alone - the generator has that one on AF7, and every pad of the real
     * SDRAM pinout is AF0. */
    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOA, ENABLE);
    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode = GPIO_MODE_ANALOG;
    gpio.Pin       = GPIO_PIN_4 | GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOA, &gpio);
    gpio.Pin       = GPIO_PIN_5;
    GPIO_InitPeripheral(GPIOH, &gpio);
    gpio.Pin       = GPIO_PIN_3;
    GPIO_InitPeripheral(GPIOC, &gpio);

    RCC_ConfigSDRAMMemClk(RCC_SDRAMMEMCLK_SRC_PLL3A, RCC_SDRAMMEMCLK_AXIDIV1);
    RCC_EnableAXIPeriphClk4(RCC_AXI_PERIPHEN_M7_SDRAM | RCC_AXI_PERIPHEN_M7_SDRAMLP, ENABLE);

    SDRAM_DeInit();
    RCC_ConfigSDRAMDelay(RCC_SDRAM_DELAY_0_2NS);
    RCC_EnableSDRAMDelayChain(ENABLE);
    SDRAM_EnableAddressRemap(DISABLE);

    /* N32H7xx UM 6.6.7-13: the registers hold cycles - 1. IS42S32800J-6 Rev C
     * p16 minima in ns are RAS/RC/RRD/RP/WR/RFC/RCD = 42/60/12/18/12/60/18,
     * which at 133 MHz is 6/8/2/3/2/8/3 cycles; the workbench validated
     * {5,8,1,2,1,8,2} as registers, i.e. one cycle over the minimum on RC and
     * RFC and keeping RC >= RAS + RP. */
    timing.RowActiveTime       = 5U;
    timing.RowCycleTime        = 8U;
    timing.RowActToRowActDelay = 1U;
    timing.PrechargeTime       = 2U;
    timing.WriteRecoveryTime   = 1U;
    timing.RefreshCycleTime    = 8U;
    timing.RAStoCASDelay       = 2U;
    SDRAM_TimingInit(&timing);

    SDRAM_RefreshIntervalInit(SDRAM_REFRESH_CYCLES);

    SDRAM_SetDeviceAddress(SDRAM_DEVICE_1, SDRAM_APP_BASE, ~(SDRAM_APP_BYTES - 1U));
    SDRAM_EnableDevice(SDRAM_DEVICE_2, DISABLE);
    SDRAM_EnableDevice(SDRAM_DEVICE_1, ENABLE);
    /* Refresh stays off until the mode register has been written. */
    SDRAM_EnableRefreshCMD(SDRAM_DEVICE_1, DISABLE);
    SDRAM_EnableAutoPrecharge(SDRAM_DEVICE_1, DISABLE);
    SDRAM_EnablePrefetchRead(SDRAM_DEVICE_1, DISABLE);
    SDRAM_EnableSOM(SDRAM_DEVICE_1, ENABLE);
#if defined(BOOT_SDRAM_READ_TEST)
    SDRAM_EnableBankInterleave(SDRAM_DEVICE_1, SDRAM_TEST_BANK ? ENABLE : DISABLE);
#else
    SDRAM_EnableBankInterleave(SDRAM_DEVICE_1, DISABLE);
#endif
    SDRAM_ConfigBusWidth(SDRAM_DEVICE_1, SDRAM_DEVICE_BUSWID_32BITS);
#if defined(BOOT_SDRAM_READ_TEST)
    SDRAM_ConfigBurstLength(SDRAM_DEVICE_1, SDRAM_TEST_BURST_SETTING);
    printf("SDRAM_TEST_INIT burst=%u bank_interleave=%u mode=%02X\r\n",
           SDRAM_TEST_BURST, SDRAM_TEST_BANK, SDRAM_TEST_MODE);
#else
    SDRAM_ConfigBurstLength(SDRAM_DEVICE_1, SDRAM_DEVICE_BURSTLEN_2);
#endif
    SDRAM_ConfigCASLatency(SDRAM_DEVICE_1, SDRAM_DEVICE_CASLTCY_3);
    SDRAM_ConfigAddress(SDRAM_DEVICE_1, SDRAM_BANK4_ROW4096_COL512);
    SDRAM_EnableWriteProtection(SDRAM_DEVICE_1, DISABLE);

    /* The part needs a settling time after power-up before it will take a
     * command, then precharge, eight auto-refreshes and the mode register -
     * burst 2, sequential, CAS 3, standard operation, burst write (0x31). */
    Sdram_Command(SDRAM_OPCODE_NONE, 0U);
    Sdram_DelayUs(1000U);

    Sdram_Command(SDRAM_OPCODE_PRECHRG, 0U);
    for (i = 0U; i < 8U; i++)
    {
        Sdram_Command(SDRAM_OPCODE_REFRESH, 0U);
    }

#if defined(BOOT_SDRAM_READ_TEST)
    Sdram_Command(SDRAM_OPCODE_LOADMODE, SDRAM_TEST_MODE);
#else
    Sdram_Command(SDRAM_OPCODE_LOADMODE, 0x31U);
#endif
    Sdram_DelayUs(1000U);

    SDRAM_EnableRefreshCMD(SDRAM_DEVICE_1, ENABLE);

    if (!Sdram_SelfTest())
    {
        g_sdram_status = SDRAM_APP_STATUS_TEST;
        return false;
    }

    printf("SDRAM ready: %u MiB at 0x%08X, %u Hz\r\n",
           (unsigned)(SDRAM_APP_BYTES / (1024U * 1024U)),
           (unsigned)SDRAM_APP_BASE, (unsigned)SDRAM_HZ);

    g_sdram_status = SDRAM_APP_STATUS_READY;
    return true;
}
