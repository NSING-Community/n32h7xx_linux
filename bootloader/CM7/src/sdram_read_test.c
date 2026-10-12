/* SPDX-License-Identifier: BSD-3-Clause */
/* Standalone destructive diagnostic, selected with make SDRAM_TEST=1.
 * Runs before the kernel is loaded. No boot medium is written. */
#if defined(BOOT_SDRAM_READ_TEST)
#include <stdio.h>
#include "n32h7xx.h"
#include "n32h7xx_dma.h"
#include "n32h7xx_mdma.h"
#include "n32h7xx_pwr.h"
#include "n32h7xx_rcc.h"
#include "n32h7xx_sdram.h"
#include "n32h7xx_usart.h"
#include "sdram_app.h"

#define WORDS (256U * 1024U)
#define CHUNK 2048U
#define SEED 0x5A5A0000U
#define HOT __attribute__((optimize("O2"), noinline))
#define IFMOD (*(volatile uint32_t *)0x51008008U)
#define ACTLR (*(volatile uint32_t *)0xE000E008U)
static uint32_t srcbuf[CHUNK] __attribute__((aligned(32)));
static uint32_t dstbuf[CHUNK] __attribute__((aligned(32)));
static volatile uint32_t sink;
static unsigned errors;
static uint32_t mdma_axi_len;

static void drain(void)
{
    __DSB();
    SDRAM->CBO = 0U; /* Flush controller writes; reading invalidates read buffers. */
    (void)SDRAM->CBO;
    __DSB();
}

static uint32_t start(void)
{
    __DSB();
    return DWT->CYCCNT;
}

static uint32_t elapsed(uint32_t t)
{
    __DSB();
    return DWT->CYCCNT - t;
}

static void line(const char *name, uint32_t bytes, uint32_t cycles)
{
    /* bytes is <= 1 MiB, so KiB * 600000 fits in 32 bits. */
    uint32_t rate = ((bytes / 1024U) * 600000U) / (cycles / 1000U + 1U);
    printf("RESULT %-19s bytes=%lu cycles=%lu MiB/s=%lu.%lu\r\n", name,
           (unsigned long)bytes, (unsigned long)cycles,
           (unsigned long)(rate / 1024U),
           (unsigned long)((rate % 1024U) * 10U / 1024U));
}

HOT static void write_words(volatile uint32_t *p)
{
    uint32_t i;
    for (i = 0U; i < WORDS; i++) p[i] = SEED + i;
}

HOT static uint32_t read_ldr(const volatile uint32_t *p)
{
    uint32_t i, a = 0U, b = 0U, c = 0U, d = 0U;
    for (i = 0U; i < WORDS; i += 4U) {
        a += p[i]; b += p[i+1U]; c += p[i+2U]; d += p[i+3U];
    }
    return a + b + c + d;
}

HOT static uint32_t read_ldm(const volatile uint32_t *p)
{
    const volatile uint32_t *end = p + WORDS;
    uint32_t sum = 0U;
    do {
        __asm volatile (
            "ldmia %[ptr]!, {r2-r9}\n"
            "add %[sum], %[sum], r2\n"
            "add %[sum], %[sum], r3\n"
            "add %[sum], %[sum], r4\n"
            "add %[sum], %[sum], r5\n"
            "add %[sum], %[sum], r6\n"
            "add %[sum], %[sum], r7\n"
            "add %[sum], %[sum], r8\n"
            "add %[sum], %[sum], r9\n"
            : [ptr] "+r" (p), [sum] "+r" (sum)
            : : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "memory", "cc");
    } while (p < end);
    return sum;
}

static void check_sum(uint32_t sum)
{
    /* Arithmetic-series sum modulo 2^32, constants avoid 64-bit helpers. */
    const uint32_t expected = (SEED * WORDS) + ((WORDS / 2U) * (WORDS - 1U));
    sink = sum;
    if (sum != expected) { errors++; printf("FAIL SUM %08lX expected=%08lX\r\n",
                            (unsigned long)sum, (unsigned long)expected); }
}

static int dma_block(uint32_t source, uint32_t destination,
                     DMA_ChBurstLenType burst, uint32_t *cycles)
{
    DMA_ChInitType cfg;
    uint32_t t;
    DMA_ChannelCmd(DMA1, DMA_CHANNEL_0, DISABLE);
    DMA_ClearChannelEventStatus(DMA1, DMA_CHANNEL_0, DMA_CH_EVENT_BLOCK_TRANSFER_COMPLETE);
    DMA_ClearChannelEventStatus(DMA1, DMA_CHANNEL_0, DMA_CH_EVENT_TRANSFER_COMPLETE);
    DMA_ClearChannelEventStatus(DMA1, DMA_CHANNEL_0, DMA_CH_EVENT_ERROR);
    DMA_ChannelStructInit(&cfg);
    cfg.SrcAddr = source; cfg.DstAddr = destination;
    cfg.SrcTfrWidth = DMA_CH_TRANSFER_WIDTH_32;
    cfg.DstTfrWidth = DMA_CH_TRANSFER_WIDTH_32;
    cfg.SrcAddrCountMode = DMA_CH_ADDRESS_COUNT_MODE_INCREMENT;
    cfg.DstAddrCountMode = DMA_CH_ADDRESS_COUNT_MODE_INCREMENT;
    cfg.SrcBurstLen = burst; cfg.DstBurstLen = burst;
    cfg.TfrTypeFlowCtrl = DMA_CH_TRANSFER_FLOW_M2M_DMA;
    cfg.BlkTfrSize = CHUNK;
    cfg.ChannelPriority = DMA_CH_PRIORITY_6;
    cfg.SrcHandshaking = DMA_CH_SRC_HANDSHAKING_HARDWARE;
    cfg.DstHandshaking = DMA_CH_DST_HANDSHAKING_HARDWARE;
    if (DMA_ChannelInit(DMA1, &cfg, DMA_CHANNEL_0) != DMA_CH_STS_OK) return 0;
    DMA_ChannelEventCmd(DMA1, DMA_CHANNEL_0, DMA_CH_EVENT_TRANSFER_COMPLETE, ENABLE);
    DMA_ChannelEventCmd(DMA1, DMA_CHANNEL_0, DMA_CH_EVENT_ERROR, ENABLE);
    t = start();
    DMA_ChannelCmd(DMA1, DMA_CHANNEL_0, ENABLE);
    while (DMA_GetChannelIntTfrStatus(DMA1, DMA_CHANNEL_0) == RESET) {
        if (DMA_GetChannelIntErrStatus(DMA1, DMA_CHANNEL_0) != RESET ||
            DWT->CYCCNT - t > 600000000U) {
            DMA_ChannelCmd(DMA1, DMA_CHANNEL_0, DISABLE);
            return 0;
        }
    }
    DMA_ChannelCmd(DMA1, DMA_CHANNEL_0, DISABLE);
    __DSB();
    *cycles = elapsed(t);
    return 1;
}

/* Timing covers enable -> transfer-complete, excluding setup and verification.
 * Each copied block is checked outside the timed interval. */
static void dma_read(const char *name, uint32_t base, DMA_ChBurstLenType burst,
                     int sdram)
{
    uint32_t offset, i, total = 0U, cycles;
    for (offset = 0U; offset < WORDS; offset += CHUNK) {
        for (i = 0U; i < CHUNK; i++) dstbuf[i] = 0xDEADBEEFU;
        if (sdram) drain();
        if (!dma_block(base + (sdram ? offset * 4U : 0U),
                       (uint32_t)dstbuf, burst, &cycles)) {
            printf("FAIL DMA %s offset=%lu\r\n", name, (unsigned long)offset);
            errors++; return;
        }
        total += cycles;
        for (i = 0U; i < CHUNK; i++) {
            if (dstbuf[i] != SEED + i + (sdram ? offset : 0U)) {
                printf("FAIL DMA DATA %s offset=%lu i=%lu got=%08lX\r\n",
                       name, (unsigned long)offset, (unsigned long)i,
                       (unsigned long)dstbuf[i]); errors++; return;
            }
        }
    }
    line(name, WORDS * 4U, total);
}

static void cpu_tests(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)SDRAM_APP_BASE;
    uint32_t t, cycles, sum;
    drain(); t = start(); sum = read_ldr(p); cycles = elapsed(t);
    check_sum(sum); line("LDR4", WORDS * 4U, cycles);
    drain(); t = start(); sum = read_ldm(p); cycles = elapsed(t);
    check_sum(sum); line("LDM8", WORDS * 4U, cycles);
}

static int mdma_block(uint32_t source, MDMA_ChTfrWidthType width,
                      MDMA_ChBurstTranLenType burst, uint32_t *cycles)
{
    MDMA_ChInitType cfg;
    uint32_t t;
    MDMA_ChannelCmd(MDMA, MDMA_CHANNEL_0, DISABLE);
    MDMA_ClearChannelInterruptStatus(MDMA, MDMA_CHANNEL_0, MDMA_CH_INT_STS_MASK);
    MDMA_ChannelStructInit(&cfg);
    cfg.SrcAddr = source; cfg.DstAddr = (uint32_t)dstbuf;
    cfg.SrcTfrWidth = width; cfg.DstTfrWidth = width;
    cfg.SrcBurstTranLen = burst; cfg.DstBurstTranLen = burst;
    cfg.SrcBurstLenEn = mdma_axi_len != 0U;
    cfg.DstBurstLenEn = mdma_axi_len != 0U;
    cfg.SrcBurstLen = mdma_axi_len;
    cfg.DstBurstLen = mdma_axi_len;
    cfg.NonPosLastWriteEn = 1U;
    cfg.BlkSize = (width == MDMA_CH_TRANSFER_WIDTH_64 ? CHUNK / 2U : CHUNK) - 1U;
    cfg.TfrTypeFlowCtrl = MDMA_CH_TRANSFER_FLOW_M2M_MDMA;
    if (MDMA_ChannelInit(MDMA, &cfg, MDMA_CHANNEL_0) != MDMA_CH_STS_OK) return 0;
    MDMA_ChannelInterruptStatusCmd(MDMA, MDMA_CHANNEL_0, MDMA_CH_INT_STS_MASK, ENABLE);
    t = start();
    MDMA_ChannelCmd(MDMA, MDMA_CHANNEL_0, ENABLE);
    while (MDMA_GetChannelInterruptStatus(MDMA, MDMA_CHANNEL_0,
                                         MDMA_CH_INT_DMA_TRF_DONE) == RESET) {
        if (DWT->CYCCNT - t > 600000000U) {
            printf("MDMA TIMEOUT status=%016llX\r\n", MDMA_GetCombinedStatus(MDMA));
            MDMA_ChannelCmd(MDMA, MDMA_CHANNEL_0, DISABLE);
            return 0;
        }
    }
    MDMA_ChannelCmd(MDMA, MDMA_CHANNEL_0, DISABLE);
    *cycles = elapsed(t);
    return 1;
}

static void mdma_read(const char *name, uint32_t base, int sdram,
                      MDMA_ChTfrWidthType width, MDMA_ChBurstTranLenType burst)
{
    uint32_t offset, i, cycles, total = 0U;
    for (offset = 0U; offset < WORDS; offset += CHUNK) {
        for (i = 0U; i < CHUNK; i++) dstbuf[i] = 0xDEADBEEFU;
        if (sdram) drain();
        if (!mdma_block(base + (sdram ? offset * 4U : 0U), width, burst, &cycles)) {
            printf("FAIL MDMA %s offset=%lu\r\n", name, (unsigned long)offset);
            errors++; return;
        }
        total += cycles;
        for (i = 0U; i < CHUNK; i++) {
            if (dstbuf[i] != SEED + i + (sdram ? offset : 0U)) {
                printf("FAIL MDMA DATA %s offset=%lu i=%lu got=%08lX\r\n", name,
                       (unsigned long)offset, (unsigned long)i,
                       (unsigned long)dstbuf[i]); errors++; return;
            }
        }
    }
    line(name, WORDS * 4U, total);
}

void SDRAM_ReadTest_Run(void)
{
    uint32_t orig_ifmod = IFMOD, orig_cfg = SDRAM->CFG1;
    uint32_t t, cycles, i, prefetch, ric, rep;
    printf("\r\nSDRAM_READ_TEST_BEGIN CCR=%08lX ACTLR=%08lX IFMOD=%08lX CFG1=%08lX\r\n",
           (unsigned long)SCB->CCR, (unsigned long)ACTLR,
           (unsigned long)orig_ifmod, (unsigned long)orig_cfg);
    if (SCB->CCR & SCB_CCR_DC_Msk) { printf("FAIL DCACHE already enabled\r\n"); return; }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    t = start(); write_words((volatile uint32_t *)SDRAM_APP_BASE);
    drain(); cycles = elapsed(t);
    line("WRITE32 drained", WORDS * 4U, cycles);
    printf("CONFIG Device original\r\n"); cpu_tests();
    MPU->CTRL = 0U; __DSB(); __ISB();
    MPU->RNR = 7U; MPU->RBAR = SDRAM_APP_BASE;
    MPU->RASR = (3U << MPU_RASR_AP_Pos) | (1U << MPU_RASR_TEX_Pos) |
                (24U << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk;
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB(); __ISB();
    RCC_EnableAHB1PeriphClk3(RCC_AHB1_PERIPHEN_M7_DMA1, ENABLE);
    RCC_EnableAHB1PeriphClk1(RCC_AHB1_PERIPHEN_M7_DMAMUX1, ENABLE);
    DMA_ControllerCmd(DMA1, ENABLE);
    PWR_MDMA_DomainEnable(ENABLE);
    RCC_EnableAXIPeriphClk1(RCC_AXI_PERIPHEN_M7_MDMA, ENABLE);
    MDMA_ControllerSoftReset(MDMA);
    MDMA_ControllerCmd(MDMA, ENABLE);
    for (i = 0U; i < CHUNK; i++) srcbuf[i] = SEED + i;
    printf("CONFIG SRAM DMA control src=%08lX dst=%08lX\r\n",
           (unsigned long)srcbuf, (unsigned long)dstbuf);
    dma_read("DMA SRAM burst8", (uint32_t)srcbuf, DMA_CH_BURST_LENGTH_8, 0);
    mdma_read("MDMA SRAM64 b128", (uint32_t)srcbuf, 0,
              MDMA_CH_TRANSFER_WIDTH_64, MDMA_CH_BURST_TRAN_LEN_128);
    for (ric = 0U; ric < 2U; ric++) {
        for (prefetch = 0U; prefetch < 2U; prefetch++) {
            drain(); IFMOD = (orig_ifmod & ~1U) | ric;
            SDRAM_EnablePrefetchRead(SDRAM_DEVICE_1, prefetch ? ENABLE : DISABLE);
            __DSB(); __ISB();
            printf("CONFIG NormalNC RIC=%lu prefetch=%lu IFMOD=%08lX CFG1=%08lX\r\n",
                   (unsigned long)ric, (unsigned long)prefetch,
                   (unsigned long)IFMOD, (unsigned long)SDRAM->CFG1);
            if (ric == 0U && prefetch == 0U) {
                t = start(); write_words((volatile uint32_t *)SDRAM_APP_BASE);
                drain(); cycles = elapsed(t);
                line("WRITE32 drained", WORDS * 4U, cycles);
            }
            for (rep = 0U; rep < 3U; rep++) {
                printf("REPEAT %lu\r\n", (unsigned long)rep);
                cpu_tests();
                dma_read("DMA SDRAM burst1", SDRAM_APP_BASE, DMA_CH_BURST_LENGTH_1, 1);
                dma_read("DMA SDRAM burst8", SDRAM_APP_BASE, DMA_CH_BURST_LENGTH_8, 1);
                mdma_read("MDMA32 b1", SDRAM_APP_BASE, 1,
                          MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_1);
                mdma_read("MDMA32 b8", SDRAM_APP_BASE, 1,
                          MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_8);
                mdma_read("MDMA32 b128", SDRAM_APP_BASE, 1,
                          MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_128);
                mdma_read("MDMA64 b8", SDRAM_APP_BASE, 1,
                          MDMA_CH_TRANSFER_WIDTH_64, MDMA_CH_BURST_TRAN_LEN_8);
                mdma_read("MDMA64 b128", SDRAM_APP_BASE, 1,
                          MDMA_CH_TRANSFER_WIDTH_64, MDMA_CH_BURST_TRAN_LEN_128);
                if (ric == 0U) {
                    mdma_axi_len = 1U;
                    mdma_read("MDMA32 AXI1", SDRAM_APP_BASE, 1,
                              MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_128);
                    mdma_axi_len = 8U;
                    mdma_read("MDMA32 AXI8", SDRAM_APP_BASE, 1,
                              MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_128);
                    mdma_axi_len = 128U;
                    mdma_read("MDMA32 AXI128", SDRAM_APP_BASE, 1,
                              MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_128);
                    mdma_axi_len = 0U;
                }
            }
        }
    }
    drain(); IFMOD = orig_ifmod; SDRAM->CFG1 = orig_cfg;
    __DSB();
    printf("SDRAM_READ_TEST_DONE errors=%u\r\n", errors);
}

#if defined(BOOT_SDRAM_SPLIT_TEST)
extern void SDRAM_TestSetBurst(uint32_t burst);
extern void SDRAM_TestSetCAS(uint32_t cas);
#if SDRAM_TEST_PLL_DIV == 5 || SDRAM_TEST_PLL_DIV == 10
/* Every next address depends on the previous read: no speculative independent
 * loads can hide buffer misses. Rings contain actual pointers, verified below. */
HOT static uint32_t chase(uint32_t p, uint32_t count)
{
    do { p = *(volatile uint32_t *)p; } while (--count);
    return p;
}

static void latency_case(const char *name, uint32_t base, uint32_t stride,
                         uint32_t nodes, uint32_t clock_cfg)
{
    uint32_t i, t, cycles, result, old_clock = RCC->SYSBUSDIV1;
    const uint32_t count = 98304U; /* divisible by all ring sizes, including 3 */
    for (i = 0U; i < nodes; i++)
        *(volatile uint32_t *)(base + i * stride) = base + ((i+1U)%nodes) * stride;
    drain();
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXC) == RESET) {}
    RCC->SYSBUSDIV1 = clock_cfg; __DSB(); __ISB();
    (void)chase(base, nodes); /* warm controller read buffers */
    t = start(); result = chase(base, count); cycles = elapsed(t);
    RCC->SYSBUSDIV1 = old_clock; __DSB(); __ISB();
    if (result != base) errors++;
    printf("LATENCY %-18s stride=%lu nodes=%lu cycles/load=%lu.%lu valid=%u\r\n",
           name, (unsigned long)stride, (unsigned long)nodes,
           (unsigned long)(cycles/count),
           (unsigned long)((cycles%count)*10U/count), result == base);
}

static void buffer_test(void)
{
    uint32_t orig_clock = RCC->SYSBUSDIV1, orig_cfg = SDRAM->CFG1;
    uint32_t mode, som, ap, pf, t, cycles, sum;
    MPU->CTRL = 0U; __DSB(); __ISB();
    MPU->RNR = 7U; MPU->RBAR = SDRAM_APP_BASE;
    MPU->RASR = (3U << MPU_RASR_AP_Pos) | (1U << MPU_RASR_TEX_Pos) |
                (24U << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk;
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB(); __ISB();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    PWR_MDMA_DomainEnable(ENABLE);
    RCC_EnableAXIPeriphClk1(RCC_AXI_PERIPHEN_M7_MDMA, ENABLE);
    MDMA_ControllerSoftReset(MDMA); MDMA_ControllerCmd(MDMA, ENABLE);
    printf("BUFFER_TEST_BEGIN SYSBUSDIV1=%08lX CFG1=%08lX\r\n",
           (unsigned long)orig_clock, (unsigned long)orig_cfg);
    for (mode = 0U; mode < 3U; mode++) {
        uint32_t clock_cfg = orig_clock;
        RCC_ClocksTypeDef clocks;
        if (mode) clock_cfg = (clock_cfg & RCC_BUSCLK_DIV_MASK) | RCC_BUSCLK_DIV4;
        if (mode == 2U) clock_cfg = (clock_cfg & RCC_AXICLK_DIV_MASK) | RCC_AXICLK_DIV4;
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXC) == RESET) {}
        RCC->SYSBUSDIV1 = clock_cfg; __DSB(); __ISB();
        RCC_GetClocksFreqValue(&clocks);
        RCC->SYSBUSDIV1 = orig_clock; __DSB(); __ISB();
        printf("CLOCK_MODE %lu SYSBUSDIV1=%08lX M7=%lu AXI=%lu AHB=%lu PLL3A=%lu\r\n",
               (unsigned long)mode, (unsigned long)clock_cfg,
               (unsigned long)clocks.M7ClkFreq, (unsigned long)clocks.AXIClkFreq,
               (unsigned long)clocks.AHB1ClkFreq, (unsigned long)clocks.PLL3AClkFreq);
        latency_case("SRAM self", (uint32_t)srcbuf, 4U, 1U, clock_cfg);
        latency_case("SDRAM self", 0xC0400000U, 4U, 1U, clock_cfg);
        latency_case("same8B pair", 0xC0400000U, 4U, 2U, clock_cfg);
        latency_case("two8B buffers", 0xC0400000U, 8U, 2U, clock_cfg);
        latency_case("three8B buffers", 0xC0400000U, 8U, 3U, clock_cfg);
        latency_case("four8B buffers", 0xC0400000U, 8U, 4U, clock_cfg);
        latency_case("row stream", 0xC0400000U, 8U, 128U, clock_cfg);
        latency_case("row crossing", 0xC0400000U, 2048U, 128U, clock_cfg);
        latency_case("bank crossing", 0xC0400000U, 0x800000U, 4U, clock_cfg);
        write_words((volatile uint32_t *)SDRAM_APP_BASE); drain();
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXC) == RESET) {}
        RCC->SYSBUSDIV1 = clock_cfg; __DSB(); __ISB();
        t = start(); sum = read_ldm((const volatile uint32_t *)SDRAM_APP_BASE);
        cycles = elapsed(t);
        RCC->SYSBUSDIV1 = orig_clock; __DSB(); __ISB();
        check_sum(sum); line("LDM clock test", WORDS*4U, cycles);
    }
    for (som = 0U; som < 2U; som++) for (ap = 0U; ap < 2U; ap++)
        for (pf = 0U; pf < 2U; pf++) {
            drain();
            SDRAM_EnableSOM(SDRAM_DEVICE_1, som ? ENABLE : DISABLE);
            SDRAM_EnableAutoPrecharge(SDRAM_DEVICE_1, ap ? ENABLE : DISABLE);
            SDRAM_EnablePrefetchRead(SDRAM_DEVICE_1, pf ? ENABLE : DISABLE);
            write_words((volatile uint32_t *)SDRAM_APP_BASE); drain();
            printf("BUFFER_CONFIG SOM=%lu AP=%lu PF=%lu CFG1=%08lX\r\n",
                   (unsigned long)som, (unsigned long)ap, (unsigned long)pf,
                   (unsigned long)SDRAM->CFG1);
            cpu_tests();
            mdma_read("MDMA buffer test", SDRAM_APP_BASE, 1,
                      MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_128);
        }
    drain(); SDRAM->CFG1 = orig_cfg;
    printf("SDRAM_READ_TEST_DONE errors=%u\r\n", errors);
}
#endif
HOT static void split_fill(volatile uint32_t *p, uint32_t seed)
{
    uint32_t i;
    for (i = 0U; i < WORDS; i++) p[i] = seed + i;
}

HOT static uint32_t split_check(const volatile uint32_t *p, uint32_t seed,
                               uint32_t *first)
{
    uint32_t i, bad = 0U;
    for (i = 0U; i < WORDS; i++) {
        if (p[i] != seed + i) {
            if (bad == 0U) *first = i;
            bad++;
        }
    }
    return bad;
}

static void split_result(uint32_t write_bl, uint32_t read_bl,
                         uint32_t seed, const char *label)
{
    const volatile uint32_t *p = (const volatile uint32_t *)0xC0400000U;
    uint32_t first = 0U, bad, i;
    SDRAM_TestSetBurst(read_bl);
    bad = split_check(p, seed, &first);
    printf("SPLIT %s writeBL=%lu readBL=%lu bad=%lu/%u first=%lu CFG=%08lX\r\n",
           label, (unsigned long)write_bl, (unsigned long)read_bl,
           (unsigned long)bad, WORDS, (unsigned long)first,
           (unsigned long)SDRAM->CFG1);
    printf("SAMPLES");
    drain();
    for (i = 0U; i < 16U; i++) printf(" %08lX", (unsigned long)p[i]);
    printf("\r\n");
    if (read_bl == 2U && bad) errors++;
}

void SDRAM_SplitTest_Run(void)
{
#if SDRAM_TEST_PLL_DIV == 5 || SDRAM_TEST_PLL_DIV == 10
    buffer_test(); return;
#endif
    uint32_t write_bl, read_bl, seed, rep;
    volatile uint32_t *p = (volatile uint32_t *)0xC0400000U;
    printf("SDRAM_SPLIT_BEGIN base=%08lX bytes=%u CCR=%08lX\r\n",
           (unsigned long)p, WORDS * 4U, (unsigned long)SCB->CCR);
    for (rep = 0U; rep < 2U; rep++) {
        printf("SPLIT_REPEAT %lu\r\n", (unsigned long)rep);
        for (write_bl = 2U; write_bl <= 8U; write_bl *= 2U) {
            seed = SEED + (write_bl << 20) + (rep << 24);
            SDRAM_TestSetBurst(write_bl);
            split_fill(p, seed); drain();
            for (read_bl = 2U; read_bl <= 8U; read_bl *= 2U)
                split_result(write_bl, read_bl, seed, "cross");
            split_result(write_bl, 2U, seed, "recover");
        }
    }
    SDRAM_TestSetBurst(2U);
#if SDRAM_TEST_PLL_DIV >= 10
    MPU->CTRL = 0U; __DSB(); __ISB();
    MPU->RNR = 7U; MPU->RBAR = SDRAM_APP_BASE;
    MPU->RASR = (3U << MPU_RASR_AP_Pos) | (1U << MPU_RASR_TEX_Pos) |
                (24U << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk;
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk;
    __DSB(); __ISB();
    PWR_MDMA_DomainEnable(ENABLE);
    RCC_EnableAXIPeriphClk1(RCC_AXI_PERIPHEN_M7_MDMA, ENABLE);
    MDMA_ControllerSoftReset(MDMA); MDMA_ControllerCmd(MDMA, ENABLE);
    for (rep = 0U; rep < 3U; rep++) {
        uint32_t cas = rep == 1U ? 2U : 3U;
        SDRAM_TestSetCAS(cas);
        write_words((volatile uint32_t *)SDRAM_APP_BASE); drain();
        printf("CAS_CONFIG cas=%lu CFG=%08lX\r\n", (unsigned long)cas,
               (unsigned long)SDRAM->CFG1);
        for (read_bl = 0U; read_bl < 3U; read_bl++) {
            uint32_t first = 0U;
            uint32_t bad = split_check((const volatile uint32_t *)SDRAM_APP_BASE, SEED, &first);
            printf("CAS_VERIFY bad=%lu first=%lu\r\n", (unsigned long)bad, (unsigned long)first);
            if (bad) errors++;
            cpu_tests();
            mdma_read("MDMA CAS test", SDRAM_APP_BASE, 1,
                      MDMA_CH_TRANSFER_WIDTH_32, MDMA_CH_BURST_TRAN_LEN_128);
        }
    }
#endif
    printf("SDRAM_READ_TEST_DONE errors=%u\r\n", errors);
}
#endif
#endif
