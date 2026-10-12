/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sdram_bench.c
 * @brief How fast the CM7 can read and write the SDRAM, and how that compares
 *        to the SRAM it is running from.
 *
 * This is the number that decides how much of a NOMMU Linux boot can live in
 * SDRAM, so it is measured the way the system actually sees the memory, not the
 * way the DRAM datasheet describes it:
 *
 *   - the 0xA0000000-0xDFFFFFFF window is Device memory in the M7's default
 *     attribute map, and this project sets no MPU region over it. So a load is
 *     a bus round trip the core waits on and a store is posted through the
 *     write buffer - nothing cached, reordered or speculated. That is the
 *     configuration the SDRAM runs in today, and its numbers are the first
 *     table below;
 *
 *   - every pass is then run a second time with an MPU region over the same
 *     window making it Normal, non-cacheable instead. Still uncached, so no
 *     cache maintenance is implied and nothing in the system has to change for
 *     it - but Normal memory may be speculated on and accessed out of order,
 *     so the core can keep several accesses in flight instead of one;
 *
 *   - and a third time with that region writable-back and the D-cache on,
 *     which is the configuration the h760_tf workbench ran this board in and
 *     the only one in which the cache can hold any of the window. This third
 *     table is a microbenchmark - the project does not run with the D-cache -
 *     and it is here because it is the question the Linux plan turns on: what
 *     the caches buy, and whether code has to be run from SDRAM uncached. The
 *     I-cache is left off for all three, so what changes between them is only
 *     how data accesses to SDRAM are treated;
 *
 *   - the SDRAM controller is in its validated configuration from
 *     sdram_app.c: 133 MHz, burst 2, prefetch read off, bank interleave off;
 *
 *   - the loops below are built at -O2, not at the project's -O0. At -O0 an
 *     "add one word to a sum" iteration costs eight or ten cycles of bookkeeping
 *     around the load, which at these access times is as much as the access
 *     itself - the figure would be mostly a measurement of -O0. The timed loops
 *     are therefore the one place in this project that asks for optimisation:
 *     they are measuring the memory, not the compiler. Note that everything
 *     else here does run at -O0, so real code in SDRAM pays its own loop
 *     overhead on top of what is printed below.
 *
 * The timed loops do nothing per access except the access itself. Verifying
 * each word as it is read would add a compare and a branch to a loop whose
 * whole cost is the bus round trip, so verification is a separate, untimed pass
 * at the end instead.
 *
 * The 64-bit passes are here to answer whether wider accesses buy anything. On
 * Device memory the architecture splits an LDRD/STRD into its two words, so the
 * expectation is that they do not; measuring is cheaper than arguing.
 *
 * DESTRUCTIVE: the first megabyte of SDRAM is overwritten, and the row-miss
 * pass reads across the whole device.
 */

/* The device header leaves __DCACHE_PRESENT at its zero default, which compiles
 * SCB_EnableDCache()/SCB_DisableDCache() out of the CMSIS core header. This is
 * the one file that needs them; see Bench_CacheEnable(). Defining it here, in
 * the one translation unit that includes the core header after it, leaves the
 * rest of the project exactly as it was. */
#define __DCACHE_PRESENT 1U

#include "sdram_bench.h"

#include <stdbool.h>
#include <stdio.h>

#include "n32h7xx.h"
#include "sdram_app.h"

/** main.c calls SysTick_Config(600000) for a 1 ms tick, i.e. a 600 MHz core. */
#define SDRAM_BENCH_CORE_HZ (600000000U)

/**
 * Footprint of the sequential passes. The controller is in burst 2, so this is
 * 512K separate burst commands - far more than enough to average out the
 * startup, and small enough that a pass takes a few milliseconds, which keeps
 * the 32-bit arithmetic in Bench_Line well away from its limits.
 */
#define SDRAM_BENCH_BYTES (1024U * 1024U)
#define SDRAM_BENCH_WORDS (SDRAM_BENCH_BYTES / 4U)

/** Word the write passes put at index i, so a read-back can be checked. */
#define SDRAM_BENCH_SEED (0x5A5A0000U)

/**
 * Stride that lands on the next row of the same bank. A row is 512 columns;
 * above the column bits come the two bank bits, so stepping down one row means
 * skipping all four banks: 4 x 512 = 2048 words = 8 KiB. Every access in that
 * pass is then a row miss - precharge and activate - against the sequential
 * passes, where consecutive bursts stay inside one row.
 */
#define SDRAM_BENCH_ROW_STRIDE (2048U)
#define SDRAM_BENCH_ROWS       (SDRAM_APP_BYTES / (SDRAM_BENCH_ROW_STRIDE * 4U))

/**
 * SRAM reference: the same 32-bit write and read loops against 32 KiB of
 * RW_SRAM, repeated 64 times. One pass alone is a few tens of thousands of
 * cycles, too short to survive the division by 1000 in Bench_Line.
 *
 * This line is also the control for the -O2 attribute below: an unoptimised
 * word loop through SRAM cannot come in under about eight cycles per access, so
 * a figure of one or two means the attribute took effect and the SDRAM numbers
 * beside it are the memory's.
 */
#define SDRAM_BENCH_SRAM_BYTES (32768U)
#define SDRAM_BENCH_SRAM_WORDS (SDRAM_BENCH_SRAM_BYTES / 4U)
#define SDRAM_BENCH_SRAM_REPS  (64U)

/** Where each pass starts. Nothing else in the project uses this megabyte. */
#define SDRAM_BENCH_BASE ((volatile uint32_t *)SDRAM_APP_BASE)

/**
 * See the file header. Spelled out per function rather than as one pragma over
 * the file, so that it is obvious which loops are being optimised and why.
 */
#define SDRAM_BENCH_HOT __attribute__((optimize("O2")))

static uint32_t g_sdram_bench_sram[SDRAM_BENCH_SRAM_WORDS];
static uint32_t g_sdram_bench_start;

/* Keeps a read pass's result alive: without a use, the compiler is free to
 * delete the loop that produced it. */
static volatile uint32_t g_sdram_bench_sink;

/**
 * The MPU region the second and third tables run under, and the Flash region
 * the third one needs. Region 7 and 6 because this project defines no regions
 * at all, so any two will do; the highest numbers keep them from accidentally
 * outranking a region someone adds later.
 */
#define SDRAM_BENCH_MPU_REGION       (7U)
#define SDRAM_BENCH_MPU_FLASH_REGION (6U)
#define SDRAM_BENCH_MPU_REGIONS      (2U)

/** Where the image is linked and is executing from; see main.c. */
#define SDRAM_BENCH_FLASH_BASE (0x15000000U)

/**
 * log2 of the region sizes: the whole 32 MiB device, and enough of Flash to
 * cover the image. A region must be aligned to its own size, and both
 * 0xC0000000 and 0x15000000 are.
 */
#define SDRAM_BENCH_MPU_SDRAM_POW2 (25U)
#define SDRAM_BENCH_MPU_FLASH_POW2 (21U)

/* MPU state saved across the sweeps; see Bench_MpuSave(). */
static uint32_t g_bench_mpu_ctrl;

/* -------------------------------------------------------------------------- */
/* Timing                                                                      */
/* -------------------------------------------------------------------------- */

static void Bench_TimerStart(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();

    g_sdram_bench_start = DWT->CYCCNT;
}

/**
 * @brief Core cycles since Bench_TimerStart().
 *
 * Zero means the counter never moved - CYCCNT lives in the debug unit, so it
 * exists only while one is attached. Callers report that rather than a
 * fabricated figure.
 */
static uint32_t Bench_TimerCycles(void)
{
    return DWT->CYCCNT - g_sdram_bench_start;
}

/**
 * @brief Print one result line.
 *
 * No 64-bit arithmetic anywhere: the linker script discards libgcc.a, so the
 * helpers a 64-bit division would need are not linked in. kib *
 * (core/1000) stays inside 32 bits for any footprint under 7 MiB, and dividing
 * the cycle count in thousands costs at most the last digit.
 */
static void Bench_Line(const char *name, uint32_t kib, uint32_t accesses, uint32_t cycles)
{
    uint32_t thousandths;
    uint32_t kibPerSec;

    if ((cycles == 0U) || (accesses == 0U))
    {
        printf("  %-26s %8u KiB        -        -\r\n", name, (unsigned)kib);
        return;
    }

    thousandths = cycles / 1000U;
    if (thousandths == 0U)
    {
        thousandths = 1U;
    }

    kibPerSec = ((kib * (SDRAM_BENCH_CORE_HZ / 1000U)) / thousandths);

    printf("  %-26s %8u KiB  %5u.%u   %6u.%u\r\n",
           name, (unsigned)kib,
           (unsigned)(kibPerSec / 1024U),
           (unsigned)(((kibPerSec % 1024U) * 10U) / 1024U),
           (unsigned)(cycles / accesses),
           (unsigned)(((cycles * 10U) / accesses) % 10U));
}

/* -------------------------------------------------------------------------- */
/* Memory attributes                                                           */
/* -------------------------------------------------------------------------- */

/** Normal, non-cacheable: TEX=001 with C and B clear, full access. */
#define SDRAM_BENCH_ATTR_NORMAL_NC \
    ((3UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos))

/** Normal, write-back, write-allocate: the only way the D-cache holds any of
 *  the window. Data only, as the workbench had it - nothing executes here. */
#define SDRAM_BENCH_ATTR_WRITE_BACK \
    (SDRAM_BENCH_ATTR_NORMAL_NC | MPU_RASR_C_Msk | MPU_RASR_B_Msk | MPU_RASR_XN_Msk)

/** Privileged read-only, non-cacheable: Flash, which is only ever read. */
#define SDRAM_BENCH_ATTR_FLASH_NC \
    ((6UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos))

typedef struct
{
    uint32_t rbar;
    uint32_t rasr;
} Bench_MpuSaved;

static Bench_MpuSaved g_bench_mpu_saved[SDRAM_BENCH_MPU_REGIONS];
static bool g_bench_mpu_saved_any;

/**
 * @brief Save both regions and the control register, and leave the MPU off.
 *
 * Only the first call does anything: the later sweeps reprogram the regions
 * themselves, and it is the state found before the first sweep that has to come
 * back. A disabled MPU is the only state in which its registers may be
 * reprogrammed, so this doubles as the "open the MPU" step for Bench_MpuSet().
 */
static void Bench_MpuSave(void)
{
    if (g_bench_mpu_saved_any)
    {
        return;
    }
    g_bench_mpu_saved_any = true;

    g_bench_mpu_ctrl = MPU->CTRL;
    MPU->CTRL = 0U;
    __DSB();
    __ISB();

    MPU->RNR = SDRAM_BENCH_MPU_FLASH_REGION;
    g_bench_mpu_saved[0].rbar = MPU->RBAR;
    g_bench_mpu_saved[0].rasr = MPU->RASR;

    MPU->RNR = SDRAM_BENCH_MPU_REGION;
    g_bench_mpu_saved[1].rbar = MPU->RBAR;
    g_bench_mpu_saved[1].rasr = MPU->RASR;
}

/** @brief Program one region; the MPU stays off until Bench_MpuOn(). */
static void Bench_MpuSet(uint32_t index, uint32_t address, uint32_t size_pow2,
                         uint32_t attributes)
{
    MPU->RNR = index;
    MPU->RBAR = address | MPU_RBAR_VALID_Msk | index;
    MPU->RASR = attributes | ((size_pow2 - 1U) << MPU_RASR_SIZE_Pos) |
                MPU_RASR_ENABLE_Msk;
}

/**
 * @brief Turn the MPU on with the default attribute map still in force.
 *
 * PRIVDEFENA is what keeps every address outside these regions running on the
 * attributes it had before - the firmware has always run on the default map.
 * We are in privileged mode and define no unprivileged region, so nothing else
 * changes.
 */
static void Bench_MpuOn(void)
{
    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();
}

/** @brief Hand both regions and the control register back exactly as found. */
static void Bench_MpuRestore(void)
{
    MPU->CTRL = 0U;
    __DSB();
    __ISB();

    MPU->RNR = SDRAM_BENCH_MPU_FLASH_REGION;
    MPU->RBAR = g_bench_mpu_saved[0].rbar;
    MPU->RASR = g_bench_mpu_saved[0].rasr;

    MPU->RNR = SDRAM_BENCH_MPU_REGION;
    MPU->RBAR = g_bench_mpu_saved[1].rbar;
    MPU->RASR = g_bench_mpu_saved[1].rasr;

    MPU->CTRL = g_bench_mpu_ctrl;
    __DSB();
    __ISB();
}

/**
 * @brief Point the SDRAM window at Normal, non-cacheable memory.
 *
 * Normal, non-cacheable is TEX=001 with C and B clear - the one combination
 * that drops the Device rules without asking for a cache. So the core stops
 * treating a load as an uninterruptible bus round trip, and nothing else in the
 * system has to know: no cache to maintain, no coherency to reason about, the
 * DMA that writes into this window sees exactly the same bytes it did before.
 */
static void Bench_MpuSdramNormal(void)
{
    Bench_MpuSave();
    Bench_MpuSet(SDRAM_BENCH_MPU_REGION, SDRAM_APP_BASE,
                 SDRAM_BENCH_MPU_SDRAM_POW2, SDRAM_BENCH_ATTR_NORMAL_NC);
    Bench_MpuOn();
}

/**
 * @brief Add the D-cache: SDRAM writable-back, Flash kept uncached.
 *
 * This is the configuration the h760_tf workbench ran this same board in, and
 * the reason for its extra region: Flash line fills on this part return
 * displaced words, so Flash has to stay non-cacheable even though it is only
 * ever read. With that region in place the D-cache can stay on for data without
 * a single Flash line ever being allocated - format strings and constants
 * included.
 *
 * The I-cache is deliberately left off, so what the next sweep shows is the
 * D-cache's effect alone: instruction fetch is identical in all three tables.
 */
static void Bench_CacheEnable(void)
{
    Bench_MpuSave();
    Bench_MpuSet(SDRAM_BENCH_MPU_FLASH_REGION, SDRAM_BENCH_FLASH_BASE,
                 SDRAM_BENCH_MPU_FLASH_POW2, SDRAM_BENCH_ATTR_FLASH_NC);
    Bench_MpuSet(SDRAM_BENCH_MPU_REGION, SDRAM_APP_BASE,
                 SDRAM_BENCH_MPU_SDRAM_POW2, SDRAM_BENCH_ATTR_WRITE_BACK);
    Bench_MpuOn();

    SCB_EnableDCache();
}

/**
 * @brief Turn the D-cache off again.
 *
 * Cleaning first, then clearing CCR.DC, then invalidating - and deliberately
 * not CMSIS's SCB_DisableDCache(), which clears CCR.DC *before* its
 * clean-and-invalidate-by-set/way loop. That order hangs on this part. The
 * loop's counters live on the stack; the stack is inside the window the cache
 * covers, one of its lines is still dirty from before, and with the cache
 * already off the write-back of that line lands on top of the counters the
 * loop is in the middle of updating. The loop then counts down from whatever
 * the stale copy said - measured here as sets arriving at 0xBF9A0B61 and ways
 * at 0xFFFFFFFF, with the core still in the loop minutes later.
 *
 * Cleaning while the cache is still on cannot do that: every write-back is
 * coherent with the loop's own stores, which go into the cache, not to memory.
 * What the epilogue re-dirties after that is dead stack below sp, and the pass
 * after the disable only invalidates - it writes nothing back, so it has
 * nothing to clobber itself with.
 *
 * The workbench this bench came from used SCB_DisableDCache() and lived: its
 * stack was inside an MPU region it had marked non-cacheable for the M4's sake,
 * so its own counters could not be written back under it. Worth remembering
 * before copying a cache teardown from one project into another.
 */
static void Bench_CacheDisable(void)
{
    SCB_CleanDCache();
    __DSB();

    SCB->CCR &= ~(uint32_t)SCB_CCR_DC_Msk;
    __DSB();
    __ISB();

    SCB_InvalidateDCache();
    __DSB();
    __ISB();
}

/* -------------------------------------------------------------------------- */
/* The passes                                                                  */
/* -------------------------------------------------------------------------- */

SDRAM_BENCH_HOT static void Bench_Write32(volatile uint32_t *p, uint32_t words, uint32_t seed)
{
    uint32_t i;

    for (i = 0U; i < words; i++)
    {
        p[i] = seed + i;
    }
}

/**
 * @brief Read @p words, four independent accumulators deep.
 *
 * The four chains matter more than they look. A single accumulator makes every
 * load wait for the add that waits for the load before it, so the loop reports
 * the *latency* of one read with nothing overlapped - on this memory, about 54
 * core cycles, a figure that says nothing about how fast data can be streamed.
 * Four chains let the core keep several reads in flight, which is what a
 * memcpy, a DMA-less driver or a page fault handler actually gets.
 *
 * Bench_Read32Dependent() below keeps the single-chain version on purpose, so
 * the two are reported side by side.
 */
SDRAM_BENCH_HOT static void Bench_Read32(const volatile uint32_t *p, uint32_t words)
{
    uint32_t i;
    uint32_t a = 0U;
    uint32_t b = 0U;
    uint32_t c = 0U;
    uint32_t d = 0U;

    for (i = 0U; i + 4U <= words; i += 4U)
    {
        a += p[i];
        b += p[i + 1U];
        c += p[i + 2U];
        d += p[i + 3U];
    }

    for (; i < words; i++)
    {
        a += p[i];
    }

    g_sdram_bench_sink = a + b + c + d;
}

/** The same read with one chain of dependencies: read latency, exposed. */
SDRAM_BENCH_HOT static void Bench_Read32Dependent(const volatile uint32_t *p, uint32_t words)
{
    uint32_t i;
    uint32_t sum = 0U;

    for (i = 0U; i < words; i++)
    {
        sum += p[i];
    }

    g_sdram_bench_sink = sum;
}

SDRAM_BENCH_HOT static void Bench_Write64(volatile uint64_t *p, uint32_t words, uint32_t seed)
{
    uint32_t i;

    for (i = 0U; i < words; i++)
    {
        uint32_t v = seed + i;

        p[i] = ((uint64_t)v << 32) | v;
    }
}

SDRAM_BENCH_HOT static void Bench_Read64(const volatile uint64_t *p, uint32_t words)
{
    uint32_t i;
    uint64_t a = 0U;
    uint64_t b = 0U;

    for (i = 0U; i + 2U <= words; i += 2U)
    {
        a += p[i];
        b += p[i + 1U];
    }

    for (; i < words; i++)
    {
        a += p[i];
    }

    /* Folded to 32 bits for printing: %llu would drag in the same missing
     * libgcc division helpers as above. */
    a += b;
    g_sdram_bench_sink = (uint32_t)a ^ (uint32_t)(a >> 32);
}

/**
 * @brief Walk @p bytes bytes one byte at a time.
 *
 * The worst case a driver can fall into: with no cache to turn a sequential
 * byte walk into line fills, every byte is its own bus round trip.
 */
SDRAM_BENCH_HOT static void Bench_Read8(const volatile uint8_t *p, uint32_t bytes)
{
    uint32_t i;
    uint32_t a = 0U;
    uint32_t b = 0U;
    uint32_t c = 0U;
    uint32_t d = 0U;

    for (i = 0U; i + 4U <= bytes; i += 4U)
    {
        a += p[i];
        b += p[i + 1U];
        c += p[i + 2U];
        d += p[i + 3U];
    }

    for (; i < bytes; i++)
    {
        a += p[i];
    }

    g_sdram_bench_sink = a + b + c + d;
}

/** Read every @p stride words, i.e. once per row when @p stride is a row. */
SDRAM_BENCH_HOT static void Bench_ReadStrided(const volatile uint32_t *p, uint32_t stride,
                                              uint32_t accesses)
{
    uint32_t i;
    uint32_t sum = 0U;

    /* One chain here, deliberately: each access lands on a different row, so
     * there is no streaming to overlap and the figure wanted is what one row
     * miss costs. */
    for (i = 0U; i < accesses; i++)
    {
        sum += *p;
        p += stride;
    }

    g_sdram_bench_sink = sum;
}

/* -------------------------------------------------------------------------- */
/* Driver                                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Write the pattern again and read it back, counting wrong words.
 *
 * Untimed, and deliberately so; see the file header. A wrong word here would
 * make every bandwidth figure above meaningless, so it is worth the two lines.
 */
static void Bench_Verify(void)
{
    volatile uint32_t *p = SDRAM_BENCH_BASE;
    uint32_t i;
    uint32_t bad = 0U;
    uint32_t first = 0U;

    Bench_Write32(p, SDRAM_BENCH_WORDS, SDRAM_BENCH_SEED);

    for (i = 0U; i < SDRAM_BENCH_WORDS; i++)
    {
        if (p[i] != (SDRAM_BENCH_SEED + i))
        {
            if (bad == 0U)
            {
                first = i;
            }
            bad++;
        }
    }

    if (bad == 0U)
    {
        printf("  pattern written back and verified: 0 of %u words wrong\r\n",
               (unsigned)SDRAM_BENCH_WORDS);
    }
    else
    {
        printf("  FAIL: %u of %u words wrong, first at word %u (0x%08X)\r\n",
               (unsigned)bad, (unsigned)SDRAM_BENCH_WORDS, (unsigned)first,
               (unsigned)(SDRAM_APP_BASE + (first * 4U)));
    }
}

/**
 * @brief Every SDRAM pass once, printed under @p label.
 *
 * Called twice, with the window's attributes changed in between; see the file
 * header. Nothing here depends on which of the two it is running under.
 */
static void Bench_Sweep(const char *label)
{
    volatile uint32_t *p = SDRAM_BENCH_BASE;
    volatile uint64_t *p64 = (volatile uint64_t *)SDRAM_BENCH_BASE;
    volatile uint8_t *p8 = (volatile uint8_t *)SDRAM_BENCH_BASE;
    uint32_t cycles;

    printf("\r\n  %s\r\n", label);
    printf("  %-26s %12s   %7s   %8s\r\n",
           "pass", "footprint", "MiB/s", "cyc/access");

    /* Writes first, so the read passes have something to walk. */
    Bench_TimerStart();
    Bench_Write32(p, SDRAM_BENCH_WORDS, SDRAM_BENCH_SEED);
    cycles = Bench_TimerCycles();
    Bench_Line("32-bit sequential write", SDRAM_BENCH_BYTES / 1024U, SDRAM_BENCH_WORDS, cycles);

    Bench_TimerStart();
    Bench_Read32(p, SDRAM_BENCH_WORDS);
    cycles = Bench_TimerCycles();
    Bench_Line("32-bit sequential read", SDRAM_BENCH_BYTES / 1024U, SDRAM_BENCH_WORDS, cycles);

    Bench_TimerStart();
    Bench_Read32Dependent(p, SDRAM_BENCH_WORDS);
    cycles = Bench_TimerCycles();
    Bench_Line("32-bit read, 1 deep", SDRAM_BENCH_BYTES / 1024U, SDRAM_BENCH_WORDS, cycles);

    Bench_TimerStart();
    Bench_Write64(p64, SDRAM_BENCH_WORDS / 2U, SDRAM_BENCH_SEED);
    cycles = Bench_TimerCycles();
    Bench_Line("64-bit sequential write", SDRAM_BENCH_BYTES / 1024U, SDRAM_BENCH_WORDS / 2U, cycles);

    Bench_TimerStart();
    Bench_Read64(p64, SDRAM_BENCH_WORDS / 2U);
    cycles = Bench_TimerCycles();
    Bench_Line("64-bit sequential read", SDRAM_BENCH_BYTES / 1024U, SDRAM_BENCH_WORDS / 2U, cycles);

    Bench_TimerStart();
    Bench_Read8(p8, SDRAM_BENCH_BYTES);
    cycles = Bench_TimerCycles();
    Bench_Line("8-bit sequential read", SDRAM_BENCH_BYTES / 1024U, SDRAM_BENCH_BYTES, cycles);

    Bench_TimerStart();
    Bench_ReadStrided(p, SDRAM_BENCH_ROW_STRIDE, SDRAM_BENCH_ROWS);
    cycles = Bench_TimerCycles();
    /* Footprint is the data carried - one word per row over the whole device -
     * not the 32 MiB of address span the pass walks. The cyc/access column is
     * the one to read here: it is the precharge-and-activate cost. */
    Bench_Line("row-miss read", (SDRAM_BENCH_ROWS * 4U) / 1024U, SDRAM_BENCH_ROWS, cycles);
}

/**
 * @brief The two 32-bit loops against SRAM, as the yardstick.
 *
 * SRAM keeps its default attributes (Normal, write-back) in both sweeps, so
 * this is printed once: it separates "the DRAM is slow" from "an uncached core
 * is slow". The D-cache is off for this project, so these are uncached SRAM
 * figures too - the difference is that SRAM has no bus to cross.
 */
static void Bench_SramReference(void)
{
    uint32_t cycles;
    uint32_t rep;

    Bench_TimerStart();
    for (rep = 0U; rep < SDRAM_BENCH_SRAM_REPS; rep++)
    {
        Bench_Write32(g_sdram_bench_sram, SDRAM_BENCH_SRAM_WORDS, SDRAM_BENCH_SEED);
    }
    cycles = Bench_TimerCycles();
    Bench_Line("SRAM 32-bit write (reference)",
               (SDRAM_BENCH_SRAM_BYTES * SDRAM_BENCH_SRAM_REPS) / 1024U,
               SDRAM_BENCH_SRAM_WORDS * SDRAM_BENCH_SRAM_REPS, cycles);

    Bench_TimerStart();
    for (rep = 0U; rep < SDRAM_BENCH_SRAM_REPS; rep++)
    {
        Bench_Read32(g_sdram_bench_sram, SDRAM_BENCH_SRAM_WORDS);
    }
    cycles = Bench_TimerCycles();
    Bench_Line("SRAM 32-bit read (reference)",
               (SDRAM_BENCH_SRAM_BYTES * SDRAM_BENCH_SRAM_REPS) / 1024U,
               SDRAM_BENCH_SRAM_WORDS * SDRAM_BENCH_SRAM_REPS, cycles);
}

void SDRAM_Bench_Run(void)
{
    printf("\r\n===== SDRAM bandwidth (core, uncached, -O2 loops) =====" "\r\n"
           "Windows: SDRAM 0x%08X, %u KiB per sequential pass, whole device per"
           " row-miss pass;" "\r\n"
           "         SRAM 0x%08X, %u KiB x %u" "\r\n"
           "Ceiling: 133 MHz x 32 bits = 507.4 MiB/s. The same passes run three"
           " times, changing only" "\r\n"
           "         the memory attributes of the SDRAM window: as this project"
           " runs it today (Device," "\r\n"
           "         every load a bus round trip the core waits on); as Normal"
           " non-cacheable (still" "\r\n"
           "         uncached, but the core may keep accesses in flight); and with"
           " the D-cache on over" "\r\n"
           "         a writable-back window. The row-miss cost and the uncached"
           " penalty are the story," "\r\n"
           "         not the DRAM. The I-cache stays off throughout, and the"
           " attributes are all put" "\r\n"
           "         back before this function returns." "\r\n",
           (unsigned)SDRAM_APP_BASE, (unsigned)(SDRAM_BENCH_BYTES / 1024U),
           (unsigned)(uintptr_t)g_sdram_bench_sram,
           (unsigned)(SDRAM_BENCH_SRAM_BYTES / 1024U), (unsigned)SDRAM_BENCH_SRAM_REPS);

    Bench_Sweep("Device, default attributes (what this project runs with today)");

    Bench_MpuSdramNormal();
    Bench_Sweep("Normal, non-cacheable (MPU region 7 over the window)");

    /*
     * A microbenchmark of the caches, not a description of this project: the
     * D-cache is on for the length of this sweep only, and the project runs
     * without it. Turning it on for good would also mean cache maintenance on
     * every DMA buffer - the SD reads below land in SDRAM - and the Flash
     * region above staying non-cacheable forever. See the report in docs/.
     */
    Bench_CacheEnable();
    Bench_Sweep("D-cache on, SDRAM writable-back (benchmark only, not this project's config)");

    Bench_CacheDisable();
    Bench_MpuRestore();

    printf("\r\n  %-26s %12s   %7s   %8s\r\n",
           "pass", "footprint", "MiB/s", "cyc/access");
    Bench_SramReference();

    Bench_Verify();
    printf("\r\n");
}
