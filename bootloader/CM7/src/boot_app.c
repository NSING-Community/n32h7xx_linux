/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file boot_app.c
 * @brief Boot policy and kernel hand-off.
 *
 * The boot medium is split into dtb / kernel / rootfs / system partitions.
 * This file reads the first two: the DTB is loaded high in the SDRAM window
 * and its address is handed to the kernel in r2, the zImage is loaded to
 * BOOT_KERNEL_LOAD_ADDR.  rootfs and system are left alone; Linux mounts them
 * through MTD.  MSC mode exposes the whole medium to the host as a raw block
 * device, so a new image can be written with a plain block-level copy.
 */
#include "boot_app.h"

#include <stdio.h>
#include <string.h>

#include "boot_storage.h"
#if defined(SPIFLASH_BENCH)
#include "spi_flash.h"
#endif
#include "sdram_app.h"
#include "n32h7xx_cfg.h"
#include "n32h7xx_gpio.h"
#include "n32h7xx_rcc.h"

/*
 * H787 internal RAM ownership (2026-10-11). Ranges below are [start, end).
 * Keep this note, Makefile/LinkFile/h787_linux_CM7.ld and the Linux board
 * DTS reserved-memory nodes in sync when changing RAM use.
 *
 * During boot/MSC:
 *   ITCM  0x00000400..0x00040000: relocated bootloader code/rodata.
 *   DTCM  0x20000000..0x20040000: linker window; not an external DMA pool.
 *   AXI   0x24000000..0x24080000: RW_SRAM linker window for data/bss/heap;
 *         initial MSP is 0x24020000 and grows down into AXI SRAM1.
 *         A linker window is NOT proof that every SRAM bank exists.
 *   AHB1  0x30015f00..0x30015f08: MSC request magic + complement, keep intact
 *         across reset; do not place stacks, heaps or DMA buffers here.
 *
 * After Linux has taken over execution, vectors and interrupts:
 *   AXI   0x24000000..0x24040000: Linux Ethernet exclusive 256 KiB coherent
 *         DMA pool (n32h787-hmi.dts, dma-pool@24000000). This deliberately
 *         reuses bootloader RAM; never run bootloader/MSC concurrently.
 *         Ring/RX buffer addresses are allocated by the Linux DMA API,
 *         not fixed subregions that another owner may reuse.
 *   MMC uses ADMA with the SDRAM coherent pool; SPI uses PIO. Neither has
 *   a dedicated internal SRAM pool yet. ITCM/DTCM are not accessible by
 *   M4, Ethernet, SDMMC or ordinary DMA1-3 (M7/MDMA access only).
 *
 * M4 remains held in reset. Its 256 KiB private + 32 KiB shared budget is
 * NOT an implemented address reservation: AHB SRAM2 0x30020000..0x30040000
 * is the private 128 KiB candidate, AHB SRAM3 0x30040000..0x30048000 is the
 * shared candidate, and the remaining private 128 KiB has no fixed address.
 * Do not allocate these candidates without reviewing the dual-core plan.
 * AHB SRAM4 0x30048000..0x30050000 (32 KiB) is only an MMC pool candidate;
 * AHB SRAM5 shares resources with FDCAN and is not assigned here.
 * TCM/AXI SRAM2/3 capacities share configurable resources: verify actual
 * partitioning before assigning the remainder; never sum their maxima.
 * These comments document ownership; they do not enforce reservations.
 * SRAM ownership was settled from the vendor memory map plus on-board
 * measurements; treat the numbers here as the record.
 */
#define BOOT_MSC_FLAG_ADDR  0x30015F00UL
#define BOOT_MSC_FLAG_MAGIC 0x544F4F42UL /* "BOOT" */

#define BOOT_KEY_PORT        GPIOG
#define BOOT_KEY_PIN         GPIO_PIN_3

#define BOOT_LOAD_CHUNK      (64U * 1024U)

/*
 * SPI NOR partition table.  The bootloader reads exactly two of the four
 * partitions: the DTB at offset 0 and the zImage behind it.  rootfs and system
 * are consumed by Linux through MTD at run time, so they are not described
 * here beyond their effect on the kernel offset.
 *
 * This table, the DTS fixed-partitions node and tools/spi_image.py
 * must agree:
 *
 *   dtb      0x000000   64 KiB
 *   kernel   0x010000    4 MiB
 *   rootfs   0x410000    3 MiB
 *   system   0x710000    remainder of the flash
 */
#define BOOT_PART_DTB_OFFSET    0x000000UL
#define BOOT_PART_DTB_MAX       (64UL * 1024UL)
#define BOOT_PART_KERNEL_OFFSET 0x010000UL
#define BOOT_PART_KERNEL_MAX    (4UL * 1024UL * 1024UL)

/*
 * The DTB is loaded high in the SDRAM window and has to stay there for the
 * whole kernel lifetime: the flat device tree keeps pointers into the original
 * blob and /sys/firmware/fdt exposes it verbatim, so the kernel keeps no
 * private copy.  This address sits below the DMA pool at 0xC1F80000 and far
 * above anything the decompressor's malloc area reaches.
 */
#define BOOT_DTB_LOAD_ADDR      0xC1E00000UL

/* Flattened device tree header: magic at 0, big-endian totalsize at 4. */
#define FDT_MAGIC               0xD00DFEEDUL
#define FDT_TOTALSIZE_OFFSET    4U

/* ARM zImage header: magic at 0x24, absolute end address at 0x2C. */
#define ZIMAGE_MAGIC            0x016f2818UL
#define ZIMAGE_MAGIC_OFFSET     0x24U
#define ZIMAGE_END_OFFSET       0x2cU

#if defined(BOOT_USART_RX_TEST)
extern __IO uint32_t mwTick;
#endif

/* Cortex-M7 maps 0xC0000000 as Device/execute-never by default, so the
 * hand-off installs MPU regions over the SDRAM window before jumping:
 *
 *   - the shared DMA pool Linux reserves for dma_alloc_coherent() stays
 *     Normal non-cacheable.  The core has no hardware cache coherency, so
 *     nothing an on-chip master writes or reads may sit in the D-cache;
 *
 *   - the rest of the window is Normal write-back, write-allocate.  The
 *     kernel image, its data and the user pages all live there, and
 *     write-back is what lets stores coalesce instead of reaching the
 *     SDRAM one access at a time.
 *
 * The bootloader runs with the D-cache off, so there are no dirty lines to
 * clean before Linux invalidates and enables the caches in its own setup. */
#define BOOT_SDRAM_MPU_REGION (6U)
#define BOOT_SDRAM_MPU_POW2   (25U)
#define BOOT_SDRAM_MPU_ATTR \
    ((3UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos) | \
     MPU_RASR_C_Msk | MPU_RASR_B_Msk)

/* The dma-pool@c1f80000 node the device tree hands to Linux. */
#define BOOT_DMA_POOL_MPU_REGION (7U)
#define BOOT_DMA_POOL_ADDR       0xC1F80000UL
#define BOOT_DMA_POOL_POW2       (19U)
#define BOOT_DMA_POOL_MPU_ATTR \
    ((3UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos))

static bool g_msc_requested;

static void Boot_KeyInit(void)
{
    GPIO_InitType gpio;

    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOG, ENABLE);
    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode = GPIO_MODE_INPUT;
    gpio.GPIO_Pull = GPIO_PULL_UP;
    gpio.Pin = BOOT_KEY_PIN;
    GPIO_InitPeripheral(BOOT_KEY_PORT, &gpio);
}

static bool Boot_KeyPressed(void)
{
    uint32_t i;

    Boot_KeyInit();
    for (i = 0U; i < 4U; i++)
    {
        if (GPIO_ReadInputDataBit(BOOT_KEY_PORT, BOOT_KEY_PIN) != RESET)
        {
            return false;
        }
        SysTick_Delayms(5U);
    }
    return true;
}

static bool Boot_FlagRequested(void)
{
    volatile uint32_t *flag = (volatile uint32_t *)(uintptr_t)BOOT_MSC_FLAG_ADDR;

    if (flag[0] != BOOT_MSC_FLAG_MAGIC ||
        flag[1] != (uint32_t)~BOOT_MSC_FLAG_MAGIC)
    {
        return false;
    }

    flag[1] = 0U;
    flag[0] = 0U;
    return true;
}

void BOOT_InputInit(void)
{
    g_msc_requested = Boot_KeyPressed();
    if (Boot_FlagRequested())
    {
        g_msc_requested = true;
    }
}

bool BOOT_MscRequested(void)
{
    return g_msc_requested;
}

#if defined(BOOT_STORAGE_SPI_FLASH)
/* sd_app.c owns _write for the SDMMC backend; the SPI backend needs its own
 * so printf() still reaches USART1. */
int _write(int fd, char *buf, int len)
{
    int i;

    (void)fd;
    for (i = 0; i < len; i++)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXDE) == RESET)
        {
        }
        USART_SendData(USART1, (uint8_t)buf[i]);
    }
    return len;
}
#endif

/** Route PA9 to USART1_TX so printf() reaches the NSLink virtual COM port. */
static void Boot_DebugInit(void)
{
    GPIO_InitType gpio;

    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOA, ENABLE);
    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode = GPIO_MODE_AF_PP;
    gpio.GPIO_Pull = GPIO_NO_PULL;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    gpio.GPIO_Current = GPIO_HS_IO_DC_2mA;
    gpio.GPIO_Alternate = GPIO_AF7; /* USART1_TX */
    gpio.Pin = GPIO_PIN_9;
    GPIO_InitPeripheral(GPIOA, &gpio);

    setvbuf(stdout, NULL, _IONBF, 0);
}

#if defined(BOOT_USART_RX_TEST)
static void Boot_UsartRxPinInit(uint32_t mode)
{
    GPIO_InitType gpio;

    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode = mode;
    gpio.GPIO_Pull = GPIO_PULL_UP;
    gpio.GPIO_Slew_Rate = GPIO_SLEW_RATE_SLOW;
    gpio.GPIO_Current = GPIO_HS_IO_DC_2mA;
    gpio.GPIO_Alternate = GPIO_AF5; /* USART1_RX */
    gpio.Pin = GPIO_PIN_10;
    GPIO_InitPeripheral(GPIOA, &gpio);
}

static void Boot_UsartRxPhase(const char *name, uint32_t mode,
                              bool fifo_enabled, bool idle,
                              uint32_t duration_ms)
{
    uint8_t data[256];
    uint32_t start;
    uint32_t count = 0U;
    uint32_t frame = 0U;
    uint32_t noise = 0U;
    uint32_t overrun = 0U;
    uint32_t i;

    Boot_UsartRxPinInit(mode);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXC) == RESET) {}
    USART_Enable(USART1, DISABLE);
    USART1->FIFO = USART_CLEAR_FIFO;
    USART1->FIFO = fifo_enabled ? USART_FIFO_ENABLE : 0U;
    (void)USART1->STS;
    (void)USART_ReceiveData(USART1);
    USART_Enable(USART1, ENABLE);

    printf("RX_READY %s: duration=%lu CTRL1=%08lX FIFO=%08lX BRCF=%08lX\r\n",
           name, (unsigned long)duration_ms, (unsigned long)USART1->CTRL1,
           (unsigned long)USART1->FIFO, (unsigned long)USART1->BRCF);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXC) == RESET) {}
    start = mwTick;

    while ((uint32_t)(mwTick - start) < duration_ms)
    {
        uint32_t status = USART1->STS;

        if (status & (USART_STS_RXDNE | USART_STS_OREF |
                      USART_STS_FEF | USART_STS_NEF | USART_STS_PEF))
        {
            uint8_t ch = (uint8_t)USART_ReceiveData(USART1);

            if (status & USART_STS_FEF) frame++;
            if (status & USART_STS_NEF) noise++;
            if (status & USART_STS_OREF) overrun++;
            if (status & USART_STS_RXDNE)
            {
                if (count < sizeof(data)) data[count] = ch;
                count++;
            }
        }
        else if (idle)
        {
            __WFI();
        }
    }

    printf("RX_RESULT %s: count=%lu frame=%lu noise=%lu overrun=%lu STS=%08lX\r\n",
           name, (unsigned long)count, (unsigned long)frame,
           (unsigned long)noise, (unsigned long)overrun,
           (unsigned long)USART1->STS);
    printf("RX_DATA %s:", name);
    for (i = 0U; i < count && i < sizeof(data); i++)
        printf(" %02X", (unsigned)data[i]);
    printf("\r\n");
}

static void Boot_UsartRxTest(void)
{
    printf("USART1 RX test: TX=PA9 AF7, RX=PA10 AF5\r\n");

    /* Buffer during reception: logging every byte would alter the timing. */
    Boot_UsartRxPhase("input_direct", GPIO_MODE_INPUT, false, false, 5000U);
    Boot_UsartRxPhase("input_fifo", GPIO_MODE_INPUT, true, false, 5000U);
    Boot_UsartRxPhase("af_direct", GPIO_MODE_AF_PP, false, false, 5000U);
    Boot_UsartRxPhase("af_fifo", GPIO_MODE_AF_PP, true, false, 5000U);
    /* Keep the vector table, handler code and stack accessible during WFI. */
    RCC_EnableAXIPeriphClk3(RCC_AXI_PERIPHEN_M7_TCMLP |
                           RCC_AXI_PERIPHEN_M7_ASRAM1LP, ENABLE);
    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOALP, ENABLE);
    RCC_EnableAPB1PeriphClk3(RCC_APB1_PERIPHEN_M7_USART1LP, DISABLE);
    Boot_UsartRxPhase("wfi_clock_off", GPIO_MODE_INPUT, true, true, 5000U);
    RCC_EnableAPB1PeriphClk3(RCC_APB1_PERIPHEN_M7_USART1LP, ENABLE);
    Boot_UsartRxPhase("wfi_clock_on", GPIO_MODE_INPUT, true, true, 5000U);
    printf("RX_TEST_DONE\r\n");
}
#endif

static void Boot_UsbClockInit(void)
{
    RCC_ConfigHse(RCC_HSE_ENABLE);
    while (RCC_WaitHseStable() != SUCCESS)
    {
    }

    /* Vendor MSC uses HSE direct, not HSE/2. */
    RCC_ConfigUSBRefClk(RCC_USBREFCLK_HSE_DIV1);
    RCC_EnableAPB5PeriphClk2(RCC_APB5_PERIPHEN_M7_RTCPCLK, ENABLE);
}

static void Boot_QuiesceInterrupts(void)
{
    uint32_t i;

    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    for (i = 0U; i < 8U; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    __DSB();
    __ISB();
}

static void Boot_UsbBspInit(void)
{
    GPIO_InitType gpio;

    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOA, ENABLE);
    RCC_EnableAHB5PeriphClk2(RCC_AHB5_PERIPHEN_M7_AFIO, ENABLE);
    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode = GPIO_MODE_AF_PP;

    gpio.GPIO_Alternate = GPIO_AF8;
    gpio.Pin = GPIO_PIN_8;
    GPIO_InitPeripheral(GPIOA, &gpio);

    gpio.GPIO_Alternate = GPIO_AF10;
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitPeripheral(GPIOA, &gpio);
}

static void Boot_UsbMscRun(void)
{
    Boot_UsbClockInit();
    Boot_UsbBspInit();
    printf("MSC mode: raw boot medium exposed as USB mass storage\r\n");
    USB_Configuration();
    for (;;)
    {
        __NOP();
    }
}

/**
 * @brief Enter the zImage with the ARM32 boot registers set up.
 *
 *   r0 = 0
 *   r1 = ~0   the "device tree supplied" machine number.  DT_MACHINE_START
 *             expands .nr to ~0; a zero here only shows up when the DTB is
 *             rejected, and then it is misreported as a bad machine ID.
 *   r2 = physical address of the DTB
 *
 * Handing the DTB over in r2 is what makes CONFIG_ARM_APPENDED_DTB
 * unnecessary: with no 0xD00DFEED at _edata the decompressor forwards r2 to
 * the kernel instead of overwriting it with an appended blob.
 */
__attribute__((naked, noreturn)) static void Boot_JumpToKernel(uint32_t entry,
                                                               uint32_t dtb)
{
    (void)entry;
    (void)dtb;
    __asm__ volatile(
        "mov r3, r0\n\t"   /* entry point, bit0 set: Cortex-M7 is Thumb only */
        "mov r2, r1\n\t"   /* r2 = DTB, taken before the mvn below reuses r1 */
        "movs r0, #0\n\t"
        "mvn r1, r0\n\t"
        "dsb\n\t"
        "isb\n\t"
        "bx r3\n\t");
}

static void Boot_MpuRegionSet(uint32_t region, uint32_t base, uint32_t attrib,
                              uint32_t pow2)
{
    MPU->RNR = region;
    MPU->RBAR = base | MPU_RBAR_VALID_Msk | region;
    MPU->RASR = attrib | ((pow2 - 1U) << MPU_RASR_SIZE_Pos) |
                MPU_RASR_ENABLE_Msk;
}

static void Boot_MpuConfigureSdram(void)
{
    MPU->CTRL = 0U;
    __DSB();
    __ISB();

#if defined(BOOT_MPU_GUARD)
    /* Prevent speculative access to unimplemented address windows.
     * Cortex-M7 erratum 1013783 can issue PLD linefills through an unmapped
     * cacheable default region even when access would fault.  A strongly
     * ordered region 0 removes that default caching path, which is what keeps
     * the idle path stable.
     * Region 0 blocks the default map; higher regions admit only board RAM,
     * the boot flash, peripheral registers and the architectural PPB.
     */
    Boot_MpuRegionSet(0U, 0U, MPU_RASR_XN_Msk, 32U);
    Boot_MpuRegionSet(1U, 0x15000000U,
                      (1UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos), 21U);
    Boot_MpuRegionSet(2U, 0x20000000U, MPU_RASR_XN_Msk |
                      (1UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos), 29U);
    Boot_MpuRegionSet(3U, 0x40000000U, MPU_RASR_XN_Msk |
                      (1UL << MPU_RASR_AP_Pos) | MPU_RASR_B_Msk | MPU_RASR_S_Msk, 29U);
    Boot_MpuRegionSet(4U, 0xE0000000U, MPU_RASR_XN_Msk |
                      (1UL << MPU_RASR_AP_Pos), 29U);
    Boot_MpuRegionSet(5U, 0U,
                      (1UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos), 20U);
#endif

    /*
     * Cortex-M7 resolves overlaps in favour of the highest region number.
     * The non-cacheable DMA pool must override the larger cacheable window.
     */
    Boot_MpuRegionSet(BOOT_DMA_POOL_MPU_REGION, BOOT_DMA_POOL_ADDR,
                      BOOT_DMA_POOL_MPU_ATTR, BOOT_DMA_POOL_POW2);
    Boot_MpuRegionSet(BOOT_SDRAM_MPU_REGION, BOOT_KERNEL_LOAD_ADDR,
                      BOOT_SDRAM_MPU_ATTR, BOOT_SDRAM_MPU_POW2);

    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();
}

static uint32_t Boot_ReadBE32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t Boot_ReadLE32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static uint32_t Boot_LoadAlign(uint32_t bytes)
{
    return (bytes + (BOOT_STORAGE_LBA_BYTES - 1U)) &
           ~(BOOT_STORAGE_LBA_BYTES - 1U);
}

/**
 * @brief Size of the DTB in the dtb partition, or 0 when it is not a valid FDT.
 *
 * The flattened device tree stores its total size as a big-endian word right
 * after the magic, so one 512-byte read sizes the whole partition.
 */
static uint32_t Boot_DtbBytes(void)
{
    static uint8_t block[BOOT_STORAGE_LBA_BYTES] __attribute__((aligned(4)));
    uint32_t total;

    if (!BOOT_StorageReadBlocks(block,
                                BOOT_PART_DTB_OFFSET / BOOT_STORAGE_LBA_BYTES, 1U))
    {
        return 0U;
    }

    if (Boot_ReadBE32(block) != FDT_MAGIC)
    {
        return 0U;
    }

    total = Boot_ReadBE32(block + FDT_TOTALSIZE_OFFSET);
    if (total < 8U || total > BOOT_PART_DTB_MAX)
    {
        return 0U;
    }

    return Boot_LoadAlign(total);
}

/**
 * @brief Size of the zImage in the kernel partition, or 0 when invalid.
 *
 * A Linux ARM zImage carries 0x016F2818 at offset 0x24 and the image length at
 * 0x2C.  With CONFIG_ZBOOT_ROM unset the compressed image links at address 0,
 * so that field is the file length rather than a RAM address (verified against
 * the release artifact: 0x353AD0 == 3488464 bytes).  The DTB lives in its own
 * partition now, so there is no appended DTB to leave slack for.
 */
static uint32_t Boot_KernelImageBytes(void)
{
    static uint8_t block[BOOT_STORAGE_LBA_BYTES] __attribute__((aligned(4)));
    uint32_t magic;
    uint32_t end;

    if (!BOOT_StorageReadBlocks(
            block, BOOT_PART_KERNEL_OFFSET / BOOT_STORAGE_LBA_BYTES, 1U))
    {
        return 0U;
    }

    magic = Boot_ReadLE32(block + ZIMAGE_MAGIC_OFFSET);
    end = Boot_ReadLE32(block + ZIMAGE_END_OFFSET);

    if (magic != ZIMAGE_MAGIC || end == 0U || end > BOOT_PART_KERNEL_MAX)
    {
        return 0U;
    }

    return Boot_LoadAlign(end);
}

/** @brief Copy one partition into SDRAM, one chunk at a time. */
static bool Boot_LoadRegion(const char *name, uint32_t offset, uint32_t bytes,
                            uint32_t load)
{
    uint32_t done = 0U;

    printf("Loading %s from SPI 0x%06X to 0x%08X (%u KiB)...\r\n",
           name, (unsigned)offset, (unsigned)load, (unsigned)(bytes / 1024U));

    while (done < bytes)
    {
        uint32_t chunk = bytes - done;
        uint8_t *dst = (uint8_t *)(uintptr_t)(load + done);

        if (chunk > BOOT_LOAD_CHUNK)
        {
            chunk = BOOT_LOAD_CHUNK;
        }

        if (!BOOT_StorageReadBlocks(dst, (offset + done) / BOOT_STORAGE_LBA_BYTES,
                                    chunk / BOOT_STORAGE_LBA_BYTES))
        {
            printf("FAIL: %s read at SPI 0x%X\r\n", name,
                   (unsigned)(offset + done));
            return false;
        }

        done += chunk;
    }

    return true;
}

/**
 * @brief Load the DTB that Linux receives in r2.
 *
 * A bad device tree must stop the boot: Linux would come up with no memory, no
 * console and no MTD, which is far harder to diagnose than a bootloader error.
 */
static bool Boot_LoadDtb(void)
{
    uint32_t bytes = Boot_DtbBytes();

    if (bytes == 0U)
    {
        printf("FAIL: no valid DTB (magic 0x%08X) at SPI 0x%06X\r\n",
               (unsigned)FDT_MAGIC, (unsigned)BOOT_PART_DTB_OFFSET);
        printf("      hold KEY1 (PG3) or set the debugger flag to reflash\r\n");
        return false;
    }

    return Boot_LoadRegion("dtb", BOOT_PART_DTB_OFFSET, bytes, BOOT_DTB_LOAD_ADDR);
}

static bool Boot_LoadKernel(void)
{
    uint32_t bytes = Boot_KernelImageBytes();

    if (bytes == 0U)
    {
        printf("FAIL: no valid zImage (magic 0x%08X) at SPI 0x%06X\r\n",
               (unsigned)ZIMAGE_MAGIC, (unsigned)BOOT_PART_KERNEL_OFFSET);
        printf("      hold KEY1 (PG3) or set the debugger flag to reflash\r\n");
        return false;
    }

    if (!Boot_LoadRegion("kernel", BOOT_PART_KERNEL_OFFSET, bytes,
                         BOOT_KERNEL_LOAD_ADDR))
    {
        return false;
    }

    printf("Kernel image loaded; handing off to zImage at 0x%08X\r\n",
           (unsigned)BOOT_KERNEL_LOAD_ADDR);
    return true;
}

void BOOT_App_Run(void)
{
    Boot_DebugInit();

#if defined(BOOT_SDRAM_READ_TEST)
    extern void SDRAM_ReadTest_Run(void);
    if (SDRAM_App_Config())
    {
#if defined(BOOT_SDRAM_SPLIT_TEST)
        extern void SDRAM_SplitTest_Run(void);
        SDRAM_SplitTest_Run();
#else
        SDRAM_ReadTest_Run();
#endif
    }
    else
    {
        printf("SDRAM_READ_TEST_DONE errors=1 init_status=%lu\r\n",
               (unsigned long)SDRAM_App_Status());
    }
    for (;;) { __NOP(); }
#endif

#if defined(BOOT_USART_RX_TEST)
    Boot_UsartRxTest();
    for (;;)
    {
        __NOP();
    }
#endif

    BOOT_InputInit();

    if (!BOOT_StorageInit())
    {
        printf("FAIL: boot storage unavailable\r\n");
        if (BOOT_MscRequested())
        {
            Boot_UsbMscRun();
        }
        for (;;)
        {
            __NOP();
        }
    }

#if defined(SPIFLASH_BENCH)
    SpiFlash_BenchmarkRun();
    for (;;)
    {
        __NOP();
    }
#endif

    if (BOOT_MscRequested())
    {
        Boot_UsbMscRun();
    }

    if (!SDRAM_App_Config())
    {
        printf("FAIL: SDRAM unavailable; cannot boot kernel\r\n");
        for (;;)
        {
            __NOP();
        }
    }

#if defined(BOOT_SD_COMPARE)
    extern void SD_Compare_Run(void);
    SD_Compare_Run();
    for (;;) { __NOP(); }
#endif

    if (!Boot_LoadDtb())
    {
        for (;;)
        {
            __NOP();
        }
    }

    if (!Boot_LoadKernel())
    {
        for (;;)
        {
            __NOP();
        }
    }

    /* earlycon resets USART1; finish shifting the final boot message first. */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXC) == RESET) {}
    Boot_MpuConfigureSdram();
    /* Exception entry accesses memory even while the core is sleeping. */
    RCC_EnableAXIPeriphClk3(RCC_AXI_PERIPHEN_M7_TCMLP |
                           RCC_AXI_PERIPHEN_M7_ASRAM1LP, ENABLE);
    RCC_EnableAXIPeriphClk4(RCC_AXI_PERIPHEN_M7_FEMCLP, ENABLE);
    Boot_QuiesceInterrupts();
    /* Start with invalid I-cache lines. BOOT_ICACHE=0 isolates decompression
     * without instruction caching; the default retains the fast boot path.
     * D-cache remains disabled until the Linux processor setup.
     */
    __DSB();
    SCB->CCR &= ~SCB_CCR_IC_Msk;
    __ISB();
    SCB->ICIALLU = 0U;
    __DSB();
    __ISB();
#if BOOT_ZIMAGE_ICACHE
    SCB->CCR |= SCB_CCR_IC_Msk;
    __DSB();
    __ISB();
#endif
    Boot_JumpToKernel(BOOT_KERNEL_LOAD_ADDR | 1U, BOOT_DTB_LOAD_ADDR);
}
