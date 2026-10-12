/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file sd_bench.c
 * @brief SD card throughput benchmark (CM7, SDMMC1, 4-bit).
 *
 * Measures sustained read and write bandwidth through the driver's own entry
 * points (SD_ReadBlocks / SD_WriteBlocks), so the numbers include everything
 * that path pays: command overhead, the card's busy poll before each transfer,
 * and - on writes - the card's internal programming time as seen by the host.
 *
 * The host runs in polling mode (SDMMC_NODMA), so these are card-plus-driver
 * numbers, not a controller ceiling.
 *
 * DESTRUCTIVE: a 4 MiB region of the card is overwritten. The card's contents
 * were confirmed disposable.
 */

#include "sd_bench.h"

#include <stdio.h>

#include "mmc_host.h"
#include "n32h7xx.h"
#include "n32h7xx_sdmmc.h"
#include "sd_app.h"
#include "sd_host.h"

/** main.c calls SysTick_Config(600000) for a 1 ms tick, i.e. a 600 MHz core. */
#define SD_BENCH_CORE_HZ (600000000U)
#define SD_BENCH_CYCLES_PER_MS (SD_BENCH_CORE_HZ / 1000U)

/**
 * Staging buffer, in SRAM on purpose: the card should be the bottleneck, not
 * the memory the CPU pulls the FIFO into.
 *
 * One chunk is one CMD18/CMD25. Beyond the data, every chunk costs a fixed
 * round of commands whichever way - a CMD13 status poll before it, the stop
 * command after it - and on this card that round is worth about half a
 * millisecond, which is why the read pass lands at ~70% of the bus rather than
 * ~95%: at 50 MHz a 32 KiB chunk is only 10 ms of data, so the round is 5% of
 * it. Doubling the chunk halves that again, but 32 KiB is what fits: RW_SRAM
 * gives 128 KiB up to _estack, and a 64 KiB buffer (plus the 64 KiB alignment
 * the SDMA boundary needs) puts _ebss past the stack.
 */
#define SD_BENCH_BUF_BYTES (32768U)
#define SD_BENCH_BUF_WORDS (SD_BENCH_BUF_BYTES / 4U)
#define SD_BENCH_CHUNK_BLKS (SD_BENCH_BUF_BYTES / FSL_SDMMC_DEFAULT_BLOCK_SIZE)

/** Words in one block - the probe transfers exactly one. */
#define SD_BENCH_PROBE_WORDS (FSL_SDMMC_DEFAULT_BLOCK_SIZE / 4U)

/** Pattern tag the probe round-trips; any value, it only has to be distinctive. */
#define SD_BENCH_PROBE_TAG (0x5A5AU)

/** Slots of the source-index map to print (the map is 8 lines per 128). */
#define SD_BENCH_MAP_WORDS (128U)

/** Total transferred per run: 4 MiB, in chunks of SD_BENCH_BUF_BYTES. */
#define SD_BENCH_BYTES (4U * 1024U * 1024U)
#define SD_BENCH_ITERS (SD_BENCH_BYTES / SD_BENCH_BUF_BYTES)

/**
 * Where the benchmark writes: 1 GiB into the card, well clear of the MBR and
 * of anything a filesystem might put there.
 */
#define SD_BENCH_START_BLK (2097152U)

/**
 * Blocks per run in the single-block comparison. 128 KiB is long enough that
 * a 1 ms tick still resolves it to a few percent, short enough that a slow
 * single-block write cannot push a measurement past the timer's range.
 */
#define SD_BENCH_SINGLE_BLKS (256U)

/**
 * Single-block transfers get their own blocks, after the multi-block region:
 * they write one block per command with a pattern tagged by block index, which
 * is not the pattern the multi-block pass leaves there, so sharing the region
 * would make the multi-block verification compare against the wrong data.
 */
#define SD_BENCH_SINGLE_START_BLK (SD_BENCH_START_BLK + (SD_BENCH_BYTES / FSL_SDMMC_DEFAULT_BLOCK_SIZE))

/**
 * SDRAM buffer for the "can the SDRAM keep up" figure - this is the path the
 * bootloader will use. SDRAM1 decodes 16 MiB from 0xC0000000 (address mask
 * 0xFF000000), so 8 MiB in is safely inside it.
 */
#define SD_BENCH_SDRAM_ADDR (0xC0800000U)
#define SD_BENCH_SDRAM_PROBE_WORDS (256U)

/* The last eight words are slack, never used by the benchmark: the DMA engine
 * runs its address counter two words past the end of a transfer, and the two
 * variables below it are the timer, so a transfer that did reach that far
 * would wreck the measurement it is being timed for. */
static uint32_t g_sd_bench_buf[SD_BENCH_BUF_WORDS + 8U];
static uint32_t g_sd_bench_start;
static uint32_t g_sd_bench_use_dwt;

/* Transfers that stalled and had to be recovered. Zero is the expected value -
 * the stop-command change is what removed these; see SD_StopMultiBlock. */
static uint32_t g_bench_stalls;

/* -------------------------------------------------------------------------- */
/* Timing                                                                      */
/* -------------------------------------------------------------------------- */

static uint32_t Bench_SdramWorks(void);
static const char *Bench_StatusName(Status_card status);
static void Bench_DumpHost(const char *tag);
static uint32_t Bench_SrcMap(const uint32_t *buf, uint32_t words, uint32_t tag);

/**
 * @brief Start a timed run.
 *
 * DWT CYCCNT is the timer of choice - the 1 ms tick cannot resolve the
 * single-block runs. CYCCNT is read through the debug unit, though, so it is
 * not guaranteed to be ticking just because the core is; probe it once and
 * fall back to the tick counter if it is dead, rather than reporting times of
 * zero. (MMC_systick_timeoutms is the same counter SysTick_Handler bumps for
 * the SD timeouts.)
 */
static void Bench_TimerStart(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (g_sd_bench_use_dwt == 0U)
    {
        uint32_t probe = DWT->CYCCNT;
        uint32_t guard;

        g_sd_bench_use_dwt = 0U;
        for (guard = 0U; guard < 1000U; guard++)
        {
            if (DWT->CYCCNT != probe)
            {
                g_sd_bench_use_dwt = 1U;
                break;
            }
        }
        if (g_sd_bench_use_dwt == 0U)
        {
            printf("  note: DWT CYCCNT not running, timing via the 1 ms tick\r\n");
        }
    }

    g_sd_bench_start = (g_sd_bench_use_dwt != 0U) ? DWT->CYCCNT : MMC_systick_timeoutms;
}

/**
 * @brief Milliseconds since the matching Bench_TimerStart().
 *
 * Cycles are subtracted in 32-bit arithmetic, which is correct modulo 2^32 -
 * so this is valid for any run shorter than 2^32 / 600 MHz = 7.16 s, which
 * every run here is by construction.
 */
static uint32_t Bench_TimerMs(void)
{
    if (g_sd_bench_use_dwt != 0U)
    {
        return (DWT->CYCCNT - g_sd_bench_start) / SD_BENCH_CYCLES_PER_MS;
    }

    return MMC_systick_timeoutms - g_sd_bench_start;
}

/** KiB/s. Not 64-bit math: 4 MiB/1024 * 1000 is comfortably inside 32 bits. */
static uint32_t Bench_KiBPerSec(uint32_t bytes, uint32_t ms)
{
    if (ms == 0U)
    {
        return 0U;
    }
    return ((bytes / 1024U) * 1000U) / ms;
}

/** Print an integer KiB/s as MiB/s with one decimal place. */
static void Bench_PrintRate(uint32_t kibPerSec)
{
    printf("%u.%u MiB/s", (unsigned)(kibPerSec / 1024U),
           (unsigned)(((kibPerSec % 1024U) * 10U) / 1024U));
}

/* -------------------------------------------------------------------------- */
/* Data pattern                                                              */
/* -------------------------------------------------------------------------- */

/** The pattern word for index @p i under @p tag. */
static uint32_t Bench_Pattern(uint32_t tag, uint32_t i)
{
    return (tag * 0x9E3779B9U) ^ (i * 0x85EBCA6BU);
}

/**
 * Deterministic pattern keyed on the word index, so a read-back can tell a
 * correct word from a stale or shifted one - which a constant fill could not.
 */
static void Bench_Fill(uint32_t *buf, uint32_t words, uint32_t tag)
{
    uint32_t i;

    for (i = 0; i < words; i++)
    {
        buf[i] = Bench_Pattern(tag, i);
    }
}

/** Words that do not match the pattern for @p tag. */
static uint32_t Bench_Check(const uint32_t *buf, uint32_t words, uint32_t tag)
{
    uint32_t i;
    uint32_t bad = 0U;

    for (i = 0; i < words; i++)
    {
        if (buf[i] != Bench_Pattern(tag, i))
        {
            bad++;
        }
    }

    return bad;
}

/** Index of the first word that does not match, or @p words if all match. */
static uint32_t Bench_FirstMismatch(const uint32_t *buf, uint32_t words, uint32_t tag)
{
    uint32_t i;

    for (i = 0; i < words; i++)
    {
        if (buf[i] != Bench_Pattern(tag, i))
        {
            return i;
        }
    }

    return words;
}

/** Print @p n words starting at @p i, as the expected pattern under @p tag. */
static void Bench_PrintExpected(uint32_t tag, uint32_t i, uint32_t n)
{
    uint32_t k;

    for (k = 0; k < n; k++)
    {
        printf("%08X ", (unsigned)Bench_Pattern(tag, i + k));
    }
}

/** Print @p n words of @p buf starting at @p i. */
static void Bench_PrintWords(const uint32_t *buf, uint32_t i, uint32_t n)
{
    uint32_t k;

    for (k = 0; k < n; k++)
    {
        printf("%08X ", (unsigned)buf[i + k]);
    }
}

/** Dump @p words words of @p buf, 8 per line. */
static void Bench_HexDump(const char *label, const uint32_t *buf, uint32_t words)
{
    uint32_t i;

    for (i = 0U; i < words; i += 8U)
    {
        printf("      %s[%3u] ", label, (unsigned)i);
        Bench_PrintWords(buf, i, 8U);
        printf("\r\n");
    }
}

/* -------------------------------------------------------------------------- */
/* Transfer runs                                                             */
/* -------------------------------------------------------------------------- */

/** Sequential multi-block read of SD_BENCH_BYTES into @p buf. */
static uint32_t Bench_MultiRead(uint32_t *buf, uint32_t *msOut)
{
    sd_card_t *card = SD_App_Card();
    uint32_t i;

    Bench_TimerStart();
    for (i = 0; i < SD_BENCH_ITERS; i++)
    {
        if (SD_ReadBlocks(card, buf, SD_BENCH_START_BLK + (i * SD_BENCH_CHUNK_BLKS),
                          SD_BENCH_CHUNK_BLKS) != Status_Success)
        {
            printf("  FAIL: multi-block read, chunk %u of %u\r\n",
                   (unsigned)i, (unsigned)SD_BENCH_ITERS);
            Bench_DumpHost("at failure");
            return 0U;
        }
    }
    *msOut = Bench_TimerMs();

    return SD_BENCH_BYTES;
}

/** Sequential multi-block write of SD_BENCH_BYTES from @p buf. */
static uint32_t Bench_MultiWrite(uint32_t *buf, uint32_t *msOut)
{
    sd_card_t *card = SD_App_Card();
    uint32_t i;
    Status_card status;

    Bench_TimerStart();
    for (i = 0; i < SD_BENCH_ITERS; i++)
    {
        Bench_Fill(buf, SD_BENCH_BUF_WORDS, i);
        status = SD_WriteBlocks(card, buf, SD_BENCH_START_BLK + (i * SD_BENCH_CHUNK_BLKS),
                                SD_BENCH_CHUNK_BLKS);
        if (status != Status_Success)
        {
            printf("  FAIL: multi-block write, chunk %u of %u, %s\r\n",
                   (unsigned)i, (unsigned)SD_BENCH_ITERS, Bench_StatusName(status));
            Bench_DumpHost("at failure");
            return 0U;
        }
    }
    *msOut = Bench_TimerMs();

    return SD_BENCH_BYTES;
}

/** One block per command, to show what the multi-block commands are worth. */
static uint32_t Bench_SingleRead(uint32_t *buf, uint32_t *msOut)
{
    sd_card_t *card = SD_App_Card();
    uint32_t i;

    Bench_TimerStart();
    for (i = 0; i < SD_BENCH_SINGLE_BLKS; i++)
    {
        if (SD_ReadBlocks(card, buf, SD_BENCH_SINGLE_START_BLK + i, 1U) != Status_Success)
        {
            printf("  FAIL: single-block read, block %u\r\n", (unsigned)i);
            return 0U;
        }
    }
    *msOut = Bench_TimerMs();

    return SD_BENCH_SINGLE_BLKS * FSL_SDMMC_DEFAULT_BLOCK_SIZE;
}

static uint32_t Bench_SingleWrite(uint32_t *buf, uint32_t *msOut)
{
    sd_card_t *card = SD_App_Card();
    uint32_t i;
    Status_card status;

    Bench_TimerStart();
    for (i = 0; i < SD_BENCH_SINGLE_BLKS; i++)
    {
        Bench_Fill(buf, SD_BENCH_BUF_WORDS, i);
        status = SD_WriteBlocks(card, buf, SD_BENCH_SINGLE_START_BLK + i, 1U);
        if (status != Status_Success)
        {
            printf("  FAIL: single-block write, block %u, %s\r\n",
                   (unsigned)i, Bench_StatusName(status));
            Bench_DumpHost("at failure");
            return 0U;
        }
    }
    *msOut = Bench_TimerMs();

    return SD_BENCH_SINGLE_BLKS * FSL_SDMMC_DEFAULT_BLOCK_SIZE;
}

/**
 * @brief Read back what a write pass wrote and compare it with the pattern.
 *
 * The rate is only worth quoting if the bytes that moved are the bytes that were
 * meant to move: this driver's whole reason for being rewritten is that it used
 * to report Success for writes it had mangled. Each write pass is verified over
 * its own region straight after it, before the next pass can overwrite it - a
 * verification that shares blocks with a later pass measures that pass instead.
 *
 * @param taggedPerUnit Non-zero if the pattern tag is the unit's own index,
 *                 which is how both write passes tag theirs.
 */
static void Bench_Verify(const char *what, uint32_t *buf, uint32_t baseBlk,
                         uint32_t blocksPerUnit, uint32_t wordsPerUnit,
                         uint32_t units, uint32_t taggedPerUnit)
{
    sd_card_t *card = SD_App_Card();
    uint32_t i;
    uint32_t bad = 0U;

    for (i = 0U; i < units; i++)
    {
        if (SD_ReadBlocks(card, buf, baseBlk + (i * blocksPerUnit),
                          blocksPerUnit) != Status_Success)
        {
            printf("  FAIL: verify read, %s %u of %u\r\n",
                   what, (unsigned)i, (unsigned)units);
            return;
        }
        bad += Bench_Check(buf, wordsPerUnit, taggedPerUnit ? i : 0U);
    }

    if (bad == 0U)
    {
        printf("  verify      %5u KiB %s read back, 0 mismatched words\r\n",
               (unsigned)((blocksPerUnit * units * FSL_SDMMC_DEFAULT_BLOCK_SIZE) / 1024U), what);
    }
    else
    {
        printf("  FAIL: verify of the %s pass found %u mismatched words\r\n",
               what, (unsigned)bad);
    }
}

/** Print one "transfer / direction / size / time / rate / share-of-ceiling" row. */
static void Bench_Row(const char *what, const char *how, uint32_t bytes, uint32_t ms,
                      uint32_t ceilingKib)
{
    uint32_t kib = Bench_KiBPerSec(bytes, ms);

    printf("  %-12s %-8s %5u KiB %6u ms  ", what, how, (unsigned)(bytes / 1024U),
           (unsigned)ms);
    Bench_PrintRate(kib);
    if (ceilingKib != 0U)
    {
        printf("   %3u%% of ceiling", (unsigned)((kib * 100U) / ceilingKib));
    }
    printf("\r\n");
}

static const char *Bench_StatusName(Status_card status)
{
    switch (status)
    {
        case Status_Success:
            return "Success";
        case Status_Fail:
            return "Fail(transfer)";
        case Status_PollingCardIdleFailed:
            return "card not idle";
        case Status_CardStatusIdle:
            return "card idle";
        case Status_CardStatusBusy:
            return "card busy";
        case Status_CardOutOfRange:
            return "out of range";
        case Status_CardNotSupportYet:
            return "not supported";
        case Status_CardSwitchFailed:
            return "switch failed";
        default:
            return "?";
    }
}

/**
 * @brief The host controller state, for when a transfer does not do what it
 *        should. TMODE carries the command index and the data-mode bits,
 *        BLKCFG the block size (low 12 bits) and count (high 16),
 *        PRESTS the transfer-active and DAT-active flags, INTSTS the flags the
 *        driver polls, and CMDRSP0 the last card response.
 */
static void Bench_DumpHost(const char *tag)
{
    printf("      %-16s TMODE=%08X BLKCFG=%08X PRESTS=%08X INTSTS=%08X RSP0=%08X\r\n",
           tag, (unsigned)SDHOST1->TMODE, (unsigned)SDHOST1->BLKCFG,
           (unsigned)SDHOST1->PRESTS, (unsigned)SDHOST1->INTSTS,
           (unsigned)SDHOST1->CMDRSP0);
    printf("      %-16s dma: INTSTS=%08X DSADD=%08X BLKCFG=%08X PRESTS=%08X\r\n",
           "", (unsigned)g_sdhost_dma_dbg[0], (unsigned)g_sdhost_dma_dbg[1],
           (unsigned)g_sdhost_dma_dbg[2], (unsigned)g_sdhost_dma_dbg[3]);
    printf("      %-16s last transfer stopped at step %u with status %u\r\n",
           "", (unsigned)g_sdhost_last_step, (unsigned)g_sdhost_last_sts);
    printf("      %-16s commands dropped so far: %u\r\n",
           "", (unsigned)g_sdhost_cmd_wait_expired);
    printf("      %-16s stop command: seen=%u waited=%u\r\n",
           "", (unsigned)g_sdhost_stopcmd_dbg[0], (unsigned)g_sdhost_stopcmd_dbg[1]);
    printf("      %-16s last cmd %u: mask=%08X pres %08X -> %08X\r\n",
           "", (unsigned)g_sdhost_cmd_dbg[0], (unsigned)g_sdhost_cmd_dbg[1],
           (unsigned)g_sdhost_cmd_dbg[2], (unsigned)g_sdhost_cmd_dbg[3]);
    /* CTRL2 bit 2 is SD Clock Enable: a stopped bus clock is one way a command
     * can be written to TMODE and then never leave the controller. */
    printf("      %-16s CTRL1=%08X CTRL2=%08X CTRLSTS=%08X\r\n",
           "", (unsigned)SDHOST1->CTRL1, (unsigned)SDHOST1->CTRL2,
           (unsigned)SDHOST1->CTRLSTS);
}

/* -------------------------------------------------------------------------- */
/* One bus configuration                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Rebuild the card after a transfer failed part-way through its data.
 *
 * A transfer that dies mid-block leaves the card in a data state, not in
 * transfer state, so every later command is refused and one failure would
 * otherwise look like a total failure of the bus. CMD0 in the bring-up resets
 * the card's state machine, so re-running the bring-up is the recovery.
 *
 * @return true if the card came back.
 */
static bool Bench_Recover(void)
{
    sd_card_t *card = SD_App_Card();

    g_bench_stalls++;

    printf("  card is left mid-data; resetting the host data line, stopping the\r\n"
           "  card, then re-running bring-up\r\n");

    /*
     * A write that stopped part-way leaves the host's data state machine
     * active, and a host with a transfer active refuses to issue any command -
     * including the CMD0 that would reset the card. So the host is reset
     * first: the DAT-line reset drops the data phase, the CMD-line reset drops
     * whatever command was stuck. Then CMD12 tells the card to stop, which is
     * the only thing that gets it out of a multi-block write it is still
     * waiting on. Re-running the bring-up after that re-establishes the bus.
     */
    SDHOST1->CTRL2 |= SDHOST_CTRL2_SWRSTDL;
    while ((SDHOST1->CTRL2 & SDHOST_CTRL2_SWRSTDL) != 0U)
    {
        /* the bit clears itself when the reset has finished */
    }

    SDHOST1->CTRL2 |= SDHOST_CTRL2_SWRSTCL;
    while ((SDHOST1->CTRL2 & SDHOST_CTRL2_SWRSTCL) != 0U)
    {
    }

    (void)SD_NormalCMD_Send(card, SDMMC_StopTransmission, 0U, CARD_ResponseTypeR1b);

    if (SD_App_Init() == false)
    {
        printf("  bring-up after the failure did not succeed; stopping here\r\n");
        return false;
    }

    return true;
}

/**
 * @brief Round-trip every transfer size, to find which ones can be trusted.
 *
 * Status alone is not enough to pick a transfer size for the throughput runs:
 * the whole reason this driver needed changing is that it reported Success for
 * writes it had mangled. So each size is written with its own pattern, read
 * back, and the words are compared. A size only counts if it both reported
 * success and gave the pattern back.
 *
 * Sizes are tried smallest first so the first failure is the smallest transfer
 * that cannot be trusted, and each round trip is preceded by the driver's own
 * wait-for-card-idle, which is what a busy card needs.
 */
static void Bench_Sizes(void)
{
    /* 32 KiB is the biggest that fits the benchmark buffer. */
    static const uint32_t sizes[] = {1U, 2U, 4U, 8U, 16U, 32U, 64U};
    sd_card_t *card = SD_App_Card();
    uint32_t idx;
    uint32_t i;
    uint32_t shownMap = 0U;

    printf("\r\n--- round trip by transfer size (write pattern, read back, compare) ---\r\n");

    for (idx = 0U; idx < (sizeof(sizes) / sizeof(sizes[0])); idx++)
    {
        uint32_t words = sizes[idx] * SD_BENCH_PROBE_WORDS;
        uint32_t tag = 0x1000U + sizes[idx];
        Status_card wstatus;
        Status_card rstatus;
        uint32_t bad;

        Bench_Fill(g_sd_bench_buf, words, tag);
        wstatus = SD_WriteBlocks(card, g_sd_bench_buf, SD_BENCH_START_BLK, sizes[idx]);

        for (i = 0U; i < words; i++)
        {
            g_sd_bench_buf[i] = 0U;
        }

        rstatus = SD_ReadBlocks(card, g_sd_bench_buf, SD_BENCH_START_BLK, sizes[idx]);
        bad = Bench_Check(g_sd_bench_buf, words, tag);

        printf("      %3u blocks (%6u B): write %-16s read %-16s %5u/%u wrong\r\n",
               (unsigned)sizes[idx],
               (unsigned)(sizes[idx] * FSL_SDMMC_DEFAULT_BLOCK_SIZE),
               Bench_StatusName(wstatus), Bench_StatusName(rstatus),
               (unsigned)bad, (unsigned)words);

        if ((bad != 0U) && (shownMap == 0U) && (wstatus == Status_Success) && (rstatus == Status_Success))
        {
            /* Success on both halves but the words are wrong: the interesting
             * case, and the one the src map explains. Only the first one is
             * dumped, to keep the output readable. */
            shownMap = 1U;
            Bench_SrcMap(g_sd_bench_buf, words, tag);
        }

        if ((wstatus != Status_Success) || (rstatus != Status_Success))
        {
            Bench_DumpHost("at failure");
            if (Bench_Recover() == false)
            {
                return;
            }
        }
    }
}

/** Quick SDRAM write/read check, so a dead SDRAM is not blamed on the SD path. */
static uint32_t Bench_SdramWorks(void)
{
    volatile uint32_t *sdram = (volatile uint32_t *)SD_BENCH_SDRAM_ADDR;
    uint32_t i;

    for (i = 0; i < SD_BENCH_SDRAM_PROBE_WORDS; i++)
    {
        sdram[i] = 0xA5A5A5A5U ^ i;
    }
    for (i = 0; i < SD_BENCH_SDRAM_PROBE_WORDS; i++)
    {
        if (sdram[i] != (0xA5A5A5A5U ^ i))
        {
            return 0U;
        }
    }

    return 1U;
}

/** Read SD_BENCH_BYTES straight into SDRAM - the bootloader's load path. */
static uint32_t Bench_SdramRead(uint32_t *msOut)
{
    sd_card_t *card = SD_App_Card();
    uint32_t *sdram = (uint32_t *)SD_BENCH_SDRAM_ADDR;
    uint32_t i;

    Bench_TimerStart();
    for (i = 0; i < SD_BENCH_ITERS; i++)
    {
        if (SD_ReadBlocks(card, sdram, SD_BENCH_START_BLK + (i * SD_BENCH_CHUNK_BLKS),
                          SD_BENCH_CHUNK_BLKS) != Status_Success)
        {
            printf("  FAIL: SDRAM read, chunk %u\r\n", (unsigned)i);
            return 0U;
        }
    }
    *msOut = Bench_TimerMs();

    return SD_BENCH_BYTES;
}

/** Every measurement, at whatever bus timing the card is currently set to. */
static void Bench_Pass(const char *title, uint32_t busHz)
{
    /** 4-bit bus, one data bit per clock edge at the rate the clock runs at. */
    uint32_t ceilingKib = (busHz / 2U) / 1024U;
    uint32_t ms;
    uint32_t bytes;
    uint32_t stallsAtStart = g_bench_stalls;

    printf("\r\n--- %s: 4-bit @ %u MHz, ceiling ", title, (unsigned)(busHz / 1000000U));
    Bench_PrintRate(ceilingKib);
    printf(" ---\r\n");

    bytes = Bench_MultiRead(g_sd_bench_buf, &ms);
    if (bytes != 0U)
    {
        Bench_Row("multi-block", "read", bytes, ms, ceilingKib);
    }

    if (Bench_SdramWorks() != 0U)
    {
        bytes = Bench_SdramRead(&ms);
        if (bytes != 0U)
        {
            Bench_Row("multi-block", "->SDRAM", bytes, ms, ceilingKib);
        }
    }
    else
    {
        printf("  note: SDRAM at 0x%08X did not hold a pattern, skipping that read\r\n",
               (unsigned)SD_BENCH_SDRAM_ADDR);
    }

    bytes = Bench_MultiWrite(g_sd_bench_buf, &ms);
    if (bytes != 0U)
    {
        Bench_Row("multi-block", "write", bytes, ms, ceilingKib);
        Bench_Verify("multi-block", g_sd_bench_buf, SD_BENCH_START_BLK,
                     SD_BENCH_CHUNK_BLKS, SD_BENCH_BUF_WORDS, SD_BENCH_ITERS, 1U);
    }

    bytes = Bench_SingleRead(g_sd_bench_buf, &ms);
    if (bytes != 0U)
    {
        Bench_Row("single-block", "read", bytes, ms, ceilingKib);
    }

    bytes = Bench_SingleWrite(g_sd_bench_buf, &ms);
    if (bytes != 0U)
    {
        Bench_Row("single-block", "write", bytes, ms, ceilingKib);
        Bench_Verify("single-block", g_sd_bench_buf, SD_BENCH_SINGLE_START_BLK,
                     1U, FSL_SDMMC_DEFAULT_BLOCK_SIZE / 4U, SD_BENCH_SINGLE_BLKS, 1U);
    }

    if (g_bench_stalls != stallsAtStart)
    {
        printf("  note: %u transfer(s) stalled and were recovered\r\n",
               (unsigned)(g_bench_stalls - stallsAtStart));
    }
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                               */
/* -------------------------------------------------------------------------- */

/** Sum of @p words words of @p buf - a compact fingerprint to compare runs. */
static uint32_t Bench_Sum(const uint32_t *buf, uint32_t words)
{
    uint32_t i;
    uint32_t sum = 0U;

    for (i = 0U; i < words; i++)
    {
        sum = (sum << 1) ^ (sum >> 31) ^ buf[i];
    }

    return sum;
}

/**
 * @brief For each word of @p buf, print which index of pattern @p tag it holds.
 *
 * This is what turns "the block read back wrong" into a specific mechanism. If
 * the words arrive shifted, or with one source word repeated, or with a run of
 * zeros, the map says so directly, and it says whether every source word made
 * it onto the wire. Pattern words are all distinct (the multiplier is odd and
 * the indices are under 128), so the lookup has a unique answer or none.
 *
 * @return number of slots holding no word of the pattern.
 */
static uint32_t Bench_SrcMap(const uint32_t *buf, uint32_t words, uint32_t tag)
{
    uint32_t k;
    uint32_t j;
    uint32_t miss = 0U;
    /* Only the first slots are printed - a long transfer would otherwise bury
     * the rest of the run in hex. */
    uint32_t printWords = (words > SD_BENCH_MAP_WORDS) ? SD_BENCH_MAP_WORDS : words;

    printf("      which pattern word each slot holds, first %u of %u ('?' = none):\r\n",
           (unsigned)printWords, (unsigned)words);
    for (k = 0U; k < printWords; k++)
    {
        uint32_t found = 0U;

        if ((k % 16U) == 0U)
        {
            printf("      [%3u]", (unsigned)k);
        }

        for (j = 0U; j < words; j++)
        {
            if (buf[k] == Bench_Pattern(tag, j))
            {
                printf(" %3u", (unsigned)j);
                found = 1U;
                break;
            }
        }

        if (found == 0U)
        {
            printf("   ?");
            miss++;
        }

        if ((k % 16U) == 15U)
        {
            printf("\r\n");
        }
    }

    return miss;
}

/**
 * @brief Write one block and read it straight back.
 *
 * Wipes the buffer before the read, so a read that returns nothing at all
 * cannot pass for a matching one.
 *
 * @param statusOut receives the write status if not NULL.
 * @return number of words that differ from the pattern just written.
 */
static uint32_t Bench_RoundTrip(uint32_t *statusOut)
{
    sd_card_t *card = SD_App_Card();
    Status_card status;
    uint32_t i;

    for (i = 0U; i < SD_BENCH_PROBE_WORDS; i++)
    {
        g_sd_bench_buf[i] = 0U;
    }

    Bench_Fill(g_sd_bench_buf, SD_BENCH_PROBE_WORDS, SD_BENCH_PROBE_TAG);
    status = SD_WriteBlocks(card, g_sd_bench_buf, SD_BENCH_START_BLK, 1U);
    if (statusOut != 0)
    {
        *statusOut = (uint32_t)status;
    }

    for (i = 0U; i < SD_BENCH_PROBE_WORDS; i++)
    {
        g_sd_bench_buf[i] = 0U;
    }

    status = SD_ReadBlocks(card, g_sd_bench_buf, SD_BENCH_START_BLK, 1U);

    return Bench_Check(g_sd_bench_buf, SD_BENCH_PROBE_WORDS, SD_BENCH_PROBE_TAG);
}

/**
 * @brief Decide whether the write path is usable, and say why if it is not.
 *
 * Two questions have to be answered before any throughput number means
 * anything:
 *
 *  1. Is the read path steady? Two reads of the same untouched block must
 *     agree, otherwise "the write was wrong" conclusions rest on a read path
 *     that races.
 *  2. Is the write corruption deterministic? The same write, repeated, must
 *     fail the same way. A deterministic map points at logic; a different map
 *     every time points at a timing race, and the two need opposite fixes.
 *
 * @return true if a block round-trips.
 */
static bool Bench_Probe(void)
{
    sd_card_t *card = SD_App_Card();
    Status_card status;
    uint32_t *bufA = &g_sd_bench_buf[0];
    uint32_t *bufB = &g_sd_bench_buf[SD_BENCH_PROBE_WORDS];
    uint32_t sums[4];
    uint32_t k;
    uint32_t diff = 0U;
    uint32_t bad;
    uint32_t badAny = 0U;
    uint32_t first;
    uint32_t pass;
    bool same = true;

    printf("\r\n--- write probe: one block at %u ---\r\n", (unsigned)SD_BENCH_START_BLK);

    /*
     * Read oracle: block 0 was written by whatever formatted this card, not by
     * this firmware, so its content is known independently. An MBR bootstrap
     * ends with the signature 55 AA at byte offsets 510/511, i.e. the top half
     * of word 127, with the partition table in the words just before it. If the
     * read path returned words shifted or repeated - which is what the write
     * path is suspected of - the signature would land in the wrong slot, and
     * the words that carry it would not read as a plausible partition table.
     * That is the difference between "the write corrupts" and "the read does",
     * and there is no way to tell those two apart from a block this firmware
     * wrote itself.
     */
    status = SD_ReadBlocks(card, bufA, 0U, 1U);
    printf("      read oracle block 0 (read -> %s):\r\n", Bench_StatusName(status));
    Bench_HexDump("mbr", &bufA[112], 16U);
    printf("      MBR signature at bytes 510/511 = %02X %02X (a real card has 55 AA)\r\n",
           (unsigned)((bufA[127] >> 16) & 0xFFU), (unsigned)((bufA[127] >> 24) & 0xFFU));

    for (k = 0U; k < SD_BENCH_PROBE_WORDS; k++)
    {
        bufA[k] = 0U;
    }

    printf("      chunk setting is %s\r\n",
           (g_sdhost_fifo_chunk_words == 0U) ? "whole block (library default)" : "partial");

    status = SD_ReadBlocks(card, bufA, SD_BENCH_START_BLK, 1U);
    printf("      read A CMD17 -> %s\r\n", Bench_StatusName(status));
    status = SD_ReadBlocks(card, bufB, SD_BENCH_START_BLK, 1U);
    printf("      read B CMD17 -> %s\r\n", Bench_StatusName(status));

    for (k = 0U; k < SD_BENCH_PROBE_WORDS; k++)
    {
        if (bufA[k] != bufB[k])
        {
            diff++;
        }
    }

    printf("      two reads of the same untouched block: %u words differ%s\r\n",
           (unsigned)diff, (diff == 0U) ? " (read path is repeatable)" : " (READ path is not stable!)");

    if (diff != 0U)
    {
        Bench_HexDump("A", bufA, SD_BENCH_PROBE_WORDS);
        Bench_HexDump("B", bufB, SD_BENCH_PROBE_WORDS);
    }

    for (pass = 0U; pass < 4U; pass++)
    {
        uint32_t statusWrite = 0U;

        bad = Bench_RoundTrip(&statusWrite);
        sums[pass] = Bench_Sum(g_sd_bench_buf, SD_BENCH_PROBE_WORDS);
        first = Bench_FirstMismatch(g_sd_bench_buf, SD_BENCH_PROBE_WORDS, SD_BENCH_PROBE_TAG);

        if (bad != 0U)
        {
            badAny = bad;
        }

        printf("      pass %u: write %-16s %3u/%u words wrong, first at %3u, readback sum %08X\r\n",
               (unsigned)(pass + 1U), Bench_StatusName((Status_card)statusWrite),
               (unsigned)bad, (unsigned)SD_BENCH_PROBE_WORDS, (unsigned)first,
               (unsigned)sums[pass]);

        if (pass != 0U && sums[pass] != sums[0])
        {
            same = false;
        }

        if (pass == 0U && bad != 0U)
        {
            uint32_t miss;

            printf("      got  ");
            Bench_PrintWords(g_sd_bench_buf, 0U, 8U);
            printf("\r\n      want ");
            Bench_PrintExpected(SD_BENCH_PROBE_TAG, 0U, 8U);
            printf("\r\n");
            miss = Bench_SrcMap(g_sd_bench_buf, SD_BENCH_PROBE_WORDS, SD_BENCH_PROBE_TAG);
            printf("      %u slots hold none of the %u pattern words\r\n",
                   (unsigned)miss, (unsigned)SD_BENCH_PROBE_WORDS);
        }

        if (bad != 0U)
        {
            Bench_DumpHost("at failure");
        }
    }

    if (badAny == 0U)
    {
        printf("      -> block round-trips\r\n");
        return true;
    }

    printf("      -> four identical writes gave %s results\r\n",
           same ? "the SAME map: deterministic, so it is logic or a fixed\n"
                  "         arrangement, not a race"
                : "DIFFERENT maps: not reproducible, so it is a timing race");

    return false;
}

void SD_Bench_Run(void)
{
    uint32_t clk;

    printf("\r\n===== SD card throughput =====\r\n");
    printf("Region : blocks %u..%u (4 MiB), contents are disposable\r\n",
           (unsigned)SD_BENCH_START_BLK,
           (unsigned)(SD_BENCH_START_BLK + (SD_BENCH_BYTES / FSL_SDMMC_DEFAULT_BLOCK_SIZE)
                      - 1U));
    printf("Method : %u KiB chunks, driver polling path (SD_ReadBlocks/SD_WriteBlocks)\r\n",
           (unsigned)(SD_BENCH_BUF_BYTES / 1024U));

    if (Bench_Probe() == false)
    {
        printf("\r\nBlock did not round-trip; skipping the throughput passes, they\r\n"
               "would only measure a broken path.\r\n");
        return;
    }

    Bench_Sizes();

    Bench_Pass("Default Speed 25 MHz", 25000000U);

    /*
     * High Speed changes two things at once - the card's bus timing, which is a
     * CMD6 the card is free to refuse, and the clock, 25 -> 50 MHz. Doing both
     * together means a failure cannot say which of the two the card objected
     * to, so the mode goes first, still at 25 MHz, and only then the clock.
     */
    clk = SD_App_ConfigureBus(SDMMC_HS, 25000000U);
    if (clk == 0U)
    {
        printf("\r\nCMD6 to SDMMC_HS rejected by the card, staying at default speed\r\n");
    }
    else
    {
        Bench_Pass("High Speed 25 MHz", clk);

        clk = SD_App_ConfigureBus(SDMMC_HS, 50000000U);
        if (clk == 0U)
        {
            printf("\r\nWARN: could not raise the High Speed clock to 50 MHz\r\n");
        }
        else
        {
            Bench_Pass("High Speed 50 MHz", clk);

            /* Leave the card as SD_App_Init() had it. */
            if (SD_App_ConfigureBus(SDMMC_DS, 25000000U) == 0U)
            {
                printf("\r\nWARN: could not switch back to default speed\r\n");
            }
        }
    }

    printf("\r\n===== SD benchmark done =====\r\n");
}
