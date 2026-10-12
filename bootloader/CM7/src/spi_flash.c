/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file spi_flash.c
 * @brief W25Q128 on XSPI2, Quad I/O reads and standard-SPI programming.
 *
 * Register layout and the HMI V1.1 pin mux follow the N32H7xx reference manual
 * (xSPI chapter) and the vendor XSPI_QUAD example for this board.
 *
 * Reads use W25Q128 fast-read-quad-I/O (0xEB) in RX_ONLY mode with 32-bit RX
 * frames. Program/erase/status use conservative standard-SPI timing, because
 * a CPU-fed page program is limited by the 16-entry TX FIFO.
 */
#include "spi_flash.h"

#include "n32h7xx.h"
#include "n32h7xx_gpio.h"
#include "n32h7xx_rcc.h"
#include "n32h7xx_xspi_v2.h"

#if defined(SPIFLASH_BENCH)
#include <stdio.h>
#endif
#include <string.h>

/* W25Q128 command set (single-line SPI). */
#define CMD_WRITE_ENABLE   0x06U
#define CMD_READ_STATUS1   0x05U
#define CMD_READ_DATA      0x03U
#define CMD_READ_STATUS2   0x35U
#define CMD_WRITE_STATUS2  0x31U
#define CMD_READ_QUAD_IO   0xebU
#define CMD_PAGE_PROGRAM   0x02U
#define CMD_SECTOR_ERASE   0x20U
#define CMD_BLOCK_ERASE_32K 0x52U

#define STATUS_WIP         0x01U
#define STATUS2_QE         0x02U

/* xSPI control bits (mirror the device header names). */
#define CTRL0_MST_MASTER       0x80000000U
#define CTRL0_SPIFRF_STANDARD  0x00000000U
#define CTRL0_TMOD_TX_AND_RX   0x00000000U
#define CTRL0_TMOD_TX_ONLY     0x00000400U
#define CTRL0_SCPOL_LOW        0x00000000U
#define CTRL0_SCPH_FIRST_EDGE  0x00000000U
#define CTRL0_SSTE_DISABLE     0x00000000U
#define CTRL0_DFS_8_BIT        0x00000007U

/* ENH_CTRL0 bit 30: pause SCK on TX FIFO underrun instead of ending the frame. */
#define ENH_CTRL0_CLKSTREN  0x40000000U

#define STS_BUSY   0x00000001U
#define STS_TXFNF  0x00000002U
#define STS_TXFE   0x00000004U
#define STS_RXFNE  0x00000008U

/* Command/status/program transfers use the vendor's conservative single-line
 * timing. Data reads use a separate baud, selected by the read path below.
 * The CPU cannot keep a 16-entry TX FIFO full at the faster baud, and a page
 * program then truncates at the first underrun. */
#define XSPI_CMD_CLKDIV  16U

/* Cap on frames clocked but not yet drained. The FIFOs are 16 entries deep
 * (the vendor threshold levels run 0..15), so this has to stay below that. */
#define XSPI_MAX_INFLIGHT 8U

#define XSPI_TIMEOUT  (2000000U)

/* Keep the FIFO data path optimized even though the project is built -O0. */
#define SPIFLASH_HOT __attribute__((optimize("O2")))

static bool g_ready;
static bool g_quad_ready;

static inline void spi_flash_hw_init(void)
{
    GPIO_InitType gpio;

    /* GPIOB/PF/PG and AFIO. The bootloader leaves GPIOB unclocked. */
    RCC_EnableAHB5PeriphClk1(RCC_AHB5_PERIPHEN_M7_GPIOB |
                             RCC_AHB5_PERIPHEN_M7_GPIOF |
                             RCC_AHB5_PERIPHEN_M7_GPIOG, ENABLE);
    RCC_EnableAHB5PeriphClk2(RCC_AHB5_PERIPHEN_M7_AFIO, ENABLE);

    /* XSPI2 peripheral clock and its low-power branch. */
    RCC_EnableAXIPeriphClk4(RCC_AXI_PERIPHEN_M7_XSPI2, ENABLE);
    RCC_EnableAXIPeriphClk4(RCC_AXI_PERIPHEN_M7_XSPI2LP, ENABLE);

    /*
     * Pulse the peripheral reset before touching any register, as the vendor
     * XSPI_DeInit() does. Unlike the reset helpers that hold a peripheral in
     * reset, RCC_EnableAXIPeriphReset4() sets and immediately clears the bit.
     */
    /* Reset masks differ from the clock-enable masks; LP has no reset bit. */
    RCC_EnableAXIPeriphReset4(RCC_AXI_PERIPHRST_XSPI2);

    /* The vendor reference sets PLL2B=300 MHz for the XSPI clock domain; the
     * SSI input itself stays on the AXI bus clock (the reset default). */
    RCC_ConfigPLL2BDivider(RCC_PLLB_DIV30);

    GPIO_InitStruct(&gpio);
    gpio.GPIO_Mode = GPIO_MODE_AF_PP;
    gpio.GPIO_Pull = GPIO_NO_PULL;
    gpio.GPIO_Slew_Rate = GPIO_SR_FAST_SLEW;
    gpio.GPIO_Current = GPIO_DC_12mA;

    /* NCS1 = PG12 (AF0). */
    gpio.GPIO_Alternate = GPIO_AF0;
    gpio.Pin = GPIO_PIN_12;
    GPIO_InitPeripheral(GPIOG, &gpio);

    /* CLK = PB2 (AF2). */
    gpio.GPIO_Alternate = GPIO_AF2;
    gpio.Pin = GPIO_PIN_2;
    GPIO_InitPeripheral(GPIOB, &gpio);

    /* IO1 = PB0 (AF1). */
    gpio.GPIO_Alternate = GPIO_AF1;
    gpio.Pin = GPIO_PIN_0;
    GPIO_InitPeripheral(GPIOB, &gpio);

    /* IO0 = PF8, IO2 = PF7, IO3 = PF6 (AF1). */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_7 | GPIO_PIN_6;
    GPIO_InitPeripheral(GPIOF, &gpio);
}

static void xspi_configure_baud(uint32_t tmod, uint32_t frames,
                                uint32_t clkdiv)
{
    XSPI_Module *x = xSPI2;

    x->SSIENR = 0U;
    x->CTRL0 = CTRL0_MST_MASTER | CTRL0_SPIFRF_STANDARD | tmod |
               CTRL0_SCPOL_LOW | CTRL0_SCPH_FIRST_EDGE |
               CTRL0_SSTE_DISABLE | CTRL0_DFS_8_BIT;
    x->CTRL1 = frames & 0xffffU;
    x->BAUD = (clkdiv << 1) & 0x0000fffeU;
    /* Vendor standard-mode setup uses half the baud divider as the receive
     * sample delay. TX_ONLY must leave it clear. */
    x->RX_DELAY = (tmod == CTRL0_TMOD_TX_ONLY) ? 0U :
                  ((clkdiv / 2U) & 0xffU);
    /* TX_ONLY transfers longer than the 16-entry FIFO must start only after
     * the vendor threshold is reached (level 15); otherwise the core starts
     * with a shallow queue and can lose bytes on page programming. TX_AND_RX
     * has to start early, so it keeps threshold 0. */
    if (tmod == CTRL0_TMOD_TX_ONLY)
    {
        uint32_t level = (frames > 16U) ? 15U :
                         ((frames != 0U) ? (frames - 1U) : 0U);

        x->TXFT = (level << 16) & 0x001f0000U;
    }
    else
    {
        x->TXFT = 0U;
    }
    x->RXFT = 0U;
    x->SLAVE_EN = 0x03U;    /* SEN[1:0]: NSS0/NSS1 mask, as in the vendor demo */
    /*
     * Clock stretching: the core pauses SCK when the TX FIFO runs dry instead
     * of ending the transfer. The vendor page-program path enables the same bit
     * (XSPI_CLOCK_STRETCH_ENABLE). Without it a 260-frame page program stops the
     * moment the 16-entry TX FIFO first empties, and the target programs only
     * the handful of bytes that happened to be queued -- measured at 10.
     */
    x->ENH_CTRL0 |= ENH_CTRL0_CLKSTREN;
    /* ICLR is read-to-clear. A store raises an AXI SLVERR on this part. */
    (void)x->ICLR;
    x->SSIENR = 1U;
}

static void xspi_configure(uint32_t tmod, uint32_t frames)
{
    xspi_configure_baud(tmod, frames, XSPI_CMD_CLKDIV);
}

static void xspi_drain_rx(void)
{
    XSPI_Module *x = xSPI2;
    uint32_t guard = XSPI_TIMEOUT;

    while ((x->STS & STS_RXFNE) && guard--)
    {
        (void)x->DAT0;
    }
}

static bool xspi_wait_idle(void)
{
    XSPI_Module *x = xSPI2;
    uint32_t guard = XSPI_TIMEOUT;

    while ((x->STS & STS_TXFE) == 0U)
    {
        if (guard-- == 0U)
        {
            return false;
        }
    }

    guard = XSPI_TIMEOUT;
    while (x->STS & STS_BUSY)
    {
        if (guard-- == 0U)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Clock @p total frames, discarding the first @p skip received bytes and
 *        copying the remainder into @p dst.
 */
static bool xspi_transfer_baud(const uint8_t *hdr, uint32_t hdr_len,
                               uint8_t *dst, uint32_t len, uint32_t clkdiv)
{
    XSPI_Module *x = xSPI2;
    uint32_t total = hdr_len + len;
    uint32_t sent = 0U;
    uint32_t got = 0U;
    uint32_t guard = XSPI_TIMEOUT;

    xspi_configure_baud(CTRL0_TMOD_TX_AND_RX, total, clkdiv);
    xspi_drain_rx();

    while (got < total)
    {
        if (sent < total && (sent - got) < XSPI_MAX_INFLIGHT &&
            (x->STS & STS_TXFNF))
        {
            x->DAT0 = (sent < hdr_len) ? hdr[sent] : 0xffU;
            sent++;
        }

        while (x->STS & STS_RXFNE)
        {
            uint8_t b = (uint8_t)x->DAT0;

            if (got >= hdr_len && dst != NULL)
            {
                dst[got - hdr_len] = b;
            }
            got++;
        }

        if (guard-- == 0U)
        {
            return false;
        }
    }

    if (!xspi_wait_idle())
    {
        return false;
    }

    xspi_drain_rx();
    return true;
}

static bool xspi_transfer(const uint8_t *hdr, uint32_t hdr_len, uint8_t *dst,
                          uint32_t len)
{
    return xspi_transfer_baud(hdr, hdr_len, dst, len, XSPI_CMD_CLKDIV);
}

/*
 * Send @p len bytes, clocking the whole run.
 *
 * TX_ONLY with clock stretching enabled, which is the vendor page-program
 * arrangement: the core pauses SCK when the TX FIFO runs dry rather than ending
 * the transfer, so a 260-frame page program survives the CPU not being able to
 * refill a 16-entry FIFO at wire speed. Without the stretch this truncated at
 * about ten bytes; with TX_AND_RX it was no better, because the abort is about
 * the transmit FIFO draining, not about the frame count.
 */
static SPIFLASH_HOT bool xspi_send(const uint8_t *buf, uint32_t len)
{
    XSPI_Module *x = xSPI2;
    uint32_t i;
    uint32_t guard = XSPI_TIMEOUT;

    xspi_configure(CTRL0_TMOD_TX_ONLY, len);
    xspi_drain_rx();

    for (i = 0U; i < len; i++)
    {
        while ((x->STS & STS_TXFNF) == 0U)
        {
            if (guard-- == 0U)
            {
                return false;
            }
        }
        x->DAT0 = buf[i];
    }

    if (!xspi_wait_idle())
    {
        return false;
    }

    xspi_drain_rx();
    return true;
}

static bool spi_flash_read_single(uint32_t addr, uint8_t *dst, uint32_t len,
                                  uint8_t command, uint32_t clkdiv)
{
    uint32_t done = 0U;
    uint32_t hdr_len = 4U;

    while (done < len)
    {
        uint32_t chunk = len - done;
        uint8_t hdr[4];

        if (chunk > 4096U)
        {
            chunk = 4096U;
        }

        hdr[0] = command;
        hdr[1] = (uint8_t)((addr >> 16) & 0xffU);
        hdr[2] = (uint8_t)((addr >> 8) & 0xffU);
        hdr[3] = (uint8_t)(addr & 0xffU);
        if (!xspi_transfer_baud(hdr, hdr_len, dst + done, chunk, clkdiv))
        {
            return false;
        }

        addr += chunk;
        done += chunk;
    }

    return true;
}

/**
 * Read with W25Q128 fast-read-quad-I/O (0xEB).
 *
 * The instruction is sent on one line, the 24-bit address on four lines, and
 * data on four lines. The controller performs the address/dummy phases from
 * the enhanced-mode registers, so the CPU only feeds the opcode and address
 * and then drains 32-bit RX frames.
 */
static SPIFLASH_HOT bool spi_flash_read_quad_io_chunk(uint32_t addr,
                                                       uint8_t *dst,
                                                       uint32_t len,
                                                       uint32_t clkdiv)
{
    XSPI_Module *x = xSPI2;
    XSPI_EnhancedInitType init;
    uint32_t words = len / 4U;
    uint32_t i;

    if (len == 0U || (len & 3U) != 0U || words > 65535U)
    {
        return false;
    }

    XSPI_Enable(x, DISABLE);
    XSPI_InitEnhancedStruct(&init);
    init.SCPH = XSPI_SCPH_FIRST_EDGE;
    init.SCPOL = XSPI_SCPOL_LOW_LEVEL;
    init.FrameFormat = XSPI_QUAD_LINE_MODE;
    init.DataFrameSize = XSPI_FRAME_SIZE_32_BIT;
    init.TransferMode = XSPI_RX_ONLY_MODE;
    init.Baudr = clkdiv;
    init.RxdSamplingEdge = XSPI_RXD_HCLK_RISING_SAMPLING;
    init.RxdSampleDelay = 3U;
    init.TransferType = XSPI_INST_SINGLE_LINE_ADDR_MULTI_LINE;
    init.AddrLen = XSPI_ADDR_LEN_24BIT;
    init.InstructLen = XSPI_INST_LEN_8BIT;
    /* W25Q128 0xEB: mode byte plus four dummy cycles. The vendor XSPI_QUAD
     * example encodes that as six controller wait cycles. */
    init.WaitCycles = XSPI_WAIT_6_CYCLES;
    init.ClockStrech = XSPI_CLOCK_STRETCH_ENABLE;
    init.DDREable = XSPI_DDR_DISABLE;
    XSPI_InitEnhanced(x, &init);

    XSPI_SetNumberOfDataFrame(x, words);
    XSPI_Slave_Select(x, XSPI_SELECT_SLAVE_1);
    XSPI_Enable(x, ENABLE);

    XSPI_ClearRxFIFO(x);
    XSPI_SetRXFIFOFullThreshold(x, XSPI_FIFO_THRESHOLD_LEVEL15);
    XSPI_SetTXStartFIFOThreshold(x, XSPI_FIFO_THRESHOLD_LEVEL1);
    XSPI_SendData(x, CMD_READ_QUAD_IO);
    XSPI_SendData(x, addr);

    if (((uintptr_t)dst & 3U) == 0U)
    {
        uint32_t *out = (uint32_t *)(void *)dst;
        uint32_t remaining = words;

        while (remaining != 0U)
        {
            uint32_t guard = XSPI_TIMEOUT;
            uint32_t burst;
            uint32_t j;

            while ((x->STS & STS_RXFNE) == 0U)
            {
                if (guard-- == 0U)
                {
                    return false;
                }
            }

            burst = x->RXFN & 0x3fU;
            if (burst == 0U)
            {
                burst = 1U;
            }
            if (burst > remaining)
            {
                burst = remaining;
            }

            for (j = 0U; j < burst; j++)
            {
                /* The controller packs each 32-bit RX frame MSB-first; the
                 * flash data is little-endian, so swap before storing. */
                *out++ = __builtin_bswap32(x->DAT0);
            }
            remaining -= burst;
        }
    }
    else
    {
        for (i = 0U; i < words; i++)
        {
            uint32_t guard = XSPI_TIMEOUT;
            uint32_t word;

            while ((x->STS & STS_RXFNE) == 0U)
            {
                if (guard-- == 0U)
                {
                    return false;
                }
            }

            word = __builtin_bswap32(x->DAT0);
            memcpy(dst + (i * 4U), &word, sizeof(word));
        }
    }

    while (x->STS & STS_BUSY)
    {
        /* The final words are already in the RX FIFO; wait for the bus only. */
    }
    XSPI_ClearRxFIFO(x);
    return true;
}

static bool spi_flash_read_quad_io(uint32_t addr, uint8_t *dst, uint32_t len,
                                   uint32_t clkdiv)
{
    uint32_t done = 0U;

    while (done < len)
    {
        uint32_t chunk = len - done;

        if (chunk > 65532U)
        {
            chunk = 65532U;
        }
        chunk &= ~3U;

        if ((((uintptr_t)(dst + done)) & 3U) != 0U && chunk != (len - done))
        {
            /* Keep the tail byte-aligned; the helper handles unaligned dst,
             * this just avoids choosing a chunk that leaves an odd tail. */
            chunk -= 4U;
        }

        if (chunk == 0U ||
            !spi_flash_read_quad_io_chunk(addr + done, dst + done, chunk,
                                           clkdiv))
        {
            return false;
        }

        done += chunk;
    }

    return true;
}


static uint32_t spi_flash_status(void)
{
    uint8_t hdr[1] = {CMD_READ_STATUS1};
    uint8_t sts = 0U;

    if (!xspi_transfer(hdr, 1U, &sts, 1U))
    {
        return 0xffffffffU;
    }

    return sts;
}

static bool spi_flash_wait_ready(void)
{
    uint32_t guard = XSPI_TIMEOUT;

    while (guard-- != 0U)
    {
        uint32_t sts = spi_flash_status();

        if (sts == 0xffffffffU)
        {
            return false;
        }
        if ((sts & STATUS_WIP) == 0U)
        {
            return true;
        }
    }

    return false;
}

static bool spi_flash_write_enable(void)
{
    uint8_t cmd = CMD_WRITE_ENABLE;

    return xspi_send(&cmd, 1U);
}

static bool spi_flash_read_status2(uint8_t *status)
{
    uint8_t cmd = CMD_READ_STATUS2;

    return xspi_transfer(&cmd, 1U, status, 1U);
}

static bool spi_flash_quad_enable(void)
{
    uint8_t status = 0U;
    uint8_t cmd[3];

    if (!spi_flash_read_status2(&status))
    {
        return false;
    }
    if ((status & STATUS2_QE) != 0U)
    {
        return true;
    }

    if (!spi_flash_wait_ready() || !spi_flash_write_enable())
    {
        return false;
    }

    /* Match the vendor W25Q128 example: command 0x31 followed by SR1 and SR2. */
    cmd[0] = CMD_WRITE_STATUS2;
    cmd[1] = 0x00U;
    cmd[2] = STATUS2_QE;
    if (!xspi_send(cmd, sizeof(cmd)) || !spi_flash_wait_ready())
    {
        return false;
    }

    return spi_flash_read_status2(&status) &&
           ((status & STATUS2_QE) != 0U);
}

bool SpiFlash_Read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    if (addr > SPIFLASH_TOTAL_BYTES || len > SPIFLASH_TOTAL_BYTES - addr)
    {
        return false;
    }

    if (g_quad_ready)
    {
        uint32_t aligned = len & ~3U;

        if (aligned != 0U &&
            !spi_flash_read_quad_io(addr, dst, aligned, 4U))
        {
            return false;
        }

        if (aligned != len &&
            !spi_flash_read_single(addr + aligned, dst + aligned,
                                   len - aligned, CMD_READ_DATA,
                                   XSPI_CMD_CLKDIV))
        {
            return false;
        }

        return true;
    }

    /* If the device has no QE support, fall back to standard single-line
     * reads at the conservative command/program baud. */
    return spi_flash_read_single(addr, dst, len, CMD_READ_DATA,
                                 XSPI_CMD_CLKDIV);
}

bool SpiFlash_Erase(uint32_t addr, uint32_t len)
{
    uint32_t end = addr + len;

    if (len == 0U || (addr % SPIFLASH_SECTOR_BYTES) != 0U ||
        end > SPIFLASH_TOTAL_BYTES)
    {
        return false;
    }

    while (addr < end)
    {
        uint8_t cmd[4];

        if (!spi_flash_wait_ready() || !spi_flash_write_enable())
        {
            return false;
        }

        cmd[0] = CMD_SECTOR_ERASE;
        cmd[1] = (uint8_t)((addr >> 16) & 0xffU);
        cmd[2] = (uint8_t)((addr >> 8) & 0xffU);
        cmd[3] = (uint8_t)(addr & 0xffU);

        if (!xspi_send(cmd, 4U) || !spi_flash_wait_ready())
        {
            return false;
        }

        addr += SPIFLASH_SECTOR_BYTES;
    }

    return true;
}

bool SpiFlash_Program(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint32_t end = addr + len;

    if ((addr % SPIFLASH_PAGE_BYTES) + len > SPIFLASH_PAGE_BYTES ||
        end > SPIFLASH_TOTAL_BYTES)
    {
        /* The caller must not cross a page boundary in one program call. */
        return false;
    }

    while (len != 0U)
    {
        uint32_t chunk = SPIFLASH_PAGE_BYTES - (addr % SPIFLASH_PAGE_BYTES);
        uint8_t buf[4U + SPIFLASH_PAGE_BYTES];
        uint32_t i;

        if (chunk > len)
        {
            chunk = len;
        }

        if (!spi_flash_wait_ready() || !spi_flash_write_enable())
        {
            return false;
        }

        buf[0] = CMD_PAGE_PROGRAM;
        buf[1] = (uint8_t)((addr >> 16) & 0xffU);
        buf[2] = (uint8_t)((addr >> 8) & 0xffU);
        buf[3] = (uint8_t)(addr & 0xffU);
        for (i = 0U; i < chunk; i++)
        {
            buf[4U + i] = src[i];
        }

        if (!xspi_send(buf, 4U + chunk) || !spi_flash_wait_ready())
        {
            return false;
        }

        addr += chunk;
        src += chunk;
        len -= chunk;
    }

    return true;
}

bool SpiFlash_ReadBlocks(uint8_t *dst, uint32_t block, uint32_t count)
{
    if (block >= SPIFLASH_BLOCK_COUNT ||
        count > SPIFLASH_BLOCK_COUNT - block)
    {
        return false;
    }

    return SpiFlash_Read(block * SPIFLASH_BLOCK_BYTES, dst,
                         count * SPIFLASH_BLOCK_BYTES);
}

static bool spi_flash_flush_sector(uint32_t sector_addr, const uint8_t *data)
{
    uint32_t i;
    uint32_t erased = 1U;

    for (i = 0U; i < SPIFLASH_SECTOR_BYTES; i++)
    {
        if (data[i] != 0xffU)
        {
            erased = 0U;
            break;
        }
    }

    if (erased == 0U && !SpiFlash_Erase(sector_addr, SPIFLASH_SECTOR_BYTES))
    {
        return false;
    }

    for (i = 0U; i < SPIFLASH_SECTOR_BYTES; i += SPIFLASH_PAGE_BYTES)
    {
        if (!SpiFlash_Program(sector_addr + i, data + i, SPIFLASH_PAGE_BYTES))
        {
            return false;
        }
    }

    return true;
}

static bool spi_flash_erase_block32(uint32_t addr)
{
    uint8_t cmd[4];

    if ((addr & 0x7fffU) != 0U ||
        !spi_flash_wait_ready() || !spi_flash_write_enable())
    {
        return false;
    }

    cmd[0] = CMD_BLOCK_ERASE_32K;
    cmd[1] = (uint8_t)((addr >> 16) & 0xffU);
    cmd[2] = (uint8_t)((addr >> 8) & 0xffU);
    cmd[3] = (uint8_t)(addr & 0xffU);

    return xspi_send(cmd, sizeof(cmd)) && spi_flash_wait_ready();
}

static bool spi_flash_program_range(uint32_t addr, const uint8_t *src,
                                    uint32_t len)
{
    while (len != 0U)
    {
        if (!SpiFlash_Program(addr, src, SPIFLASH_PAGE_BYTES))
        {
            return false;
        }

        addr += SPIFLASH_PAGE_BYTES;
        src += SPIFLASH_PAGE_BYTES;
        len -= SPIFLASH_PAGE_BYTES;
    }

    return true;
}

bool SpiFlash_WriteBlocks(const uint8_t *src, uint32_t block, uint32_t count)
{
    /*
     * NOR flash can only clear bits on program, so a logical block write is a
     * read-modify-write of its whole 4 KiB sector. The sector is staged in RAM
     * and erased/programmed once, when the write moves on to the next sector.
     */
    static uint8_t sector[SPIFLASH_SECTOR_BYTES] __attribute__((aligned(4)));
    uint32_t addr;
    uint32_t bytes;

    if (block >= SPIFLASH_BLOCK_COUNT ||
        count > SPIFLASH_BLOCK_COUNT - block)
    {
        return false;
    }

    addr = block * SPIFLASH_BLOCK_BYTES;
    bytes = count * SPIFLASH_BLOCK_BYTES;

    while (bytes != 0U)
    {
        uint32_t sector_addr;
        uint32_t in_sector;
        uint32_t chunk;

        /* A fully-covered, 32 KiB-aligned run can use one block erase instead
         * of eight individual 4 KiB sector erases. */
        if ((addr & 0x7fffU) == 0U && bytes >= (32U * 1024U))
        {
            if (!spi_flash_erase_block32(addr) ||
                !spi_flash_program_range(addr, src, 32U * 1024U))
            {
                return false;
            }

            addr += 32U * 1024U;
            src += 32U * 1024U;
            bytes -= 32U * 1024U;
            continue;
        }

        sector_addr = addr & ~(SPIFLASH_SECTOR_BYTES - 1U);
        in_sector = addr - sector_addr;
        chunk = SPIFLASH_SECTOR_BYTES - in_sector;
        if (chunk > bytes)
        {
            chunk = bytes;
        }

        if (in_sector == 0U && chunk == SPIFLASH_SECTOR_BYTES)
        {
            if (!SpiFlash_Erase(sector_addr, SPIFLASH_SECTOR_BYTES) ||
                !spi_flash_program_range(sector_addr, src,
                                         SPIFLASH_SECTOR_BYTES))
            {
                return false;
            }
        }
        else
        {
            if (!SpiFlash_Read(sector_addr, sector, SPIFLASH_SECTOR_BYTES))
            {
                return false;
            }
            memcpy(sector + in_sector, src, chunk);
            if (!spi_flash_flush_sector(sector_addr, sector))
            {
                return false;
            }
        }

        addr += chunk;
        src += chunk;
        bytes -= chunk;
    }

    return true;
}

bool SpiFlash_Init(void)
{
    uint32_t id_guard = XSPI_TIMEOUT;

    spi_flash_hw_init();
    xspi_configure(CTRL0_TMOD_TX_AND_RX, 0U);

    /* Wait out any transfer left running by a previous stage. */
    while (xSPI2->STS & STS_BUSY)
    {
        if (id_guard-- == 0U)
        {
            g_ready = false;
            return false;
        }
    }

    g_ready = spi_flash_wait_ready();
    g_quad_ready = g_ready && spi_flash_quad_enable();

    return g_ready;
}

#if defined(SPIFLASH_BENCH)

/* Keep the benchmark clear of the kernel image and clear of the W25Q128's
 * top-of-device block-protect region, which leaves the last sectors read-only
 * on some parts. */
#define BENCH_REGION_BYTES  (128U * 1024U)
#define BENCH_REGION_ADDR   (8U * 1024U * 1024U)
#define BENCH_BUF_BYTES     SPIFLASH_SECTOR_BYTES
#define BENCH_WRITE32_BYTES (32U * 1024U)
#define BENCH_CORE_HZ       600000000U

static uint8_t bench_src[BENCH_BUF_BYTES] __attribute__((aligned(4)));
static uint8_t bench_dst[BENCH_BUF_BYTES] __attribute__((aligned(4)));
/* In benchmark mode USB MSC is not started, so its 64 KiB BOT buffer is a
 * convenient source buffer and avoids pushing the benchmark BSS past _estack. */
extern uint8_t USBD1_MSC_BOT_Data[];

/* Results are also kept in RAM so the exact figures can be read over SWD even
 * if the USB CDC console drops characters. */
volatile uint32_t g_spi_bench_cycles[8];
volatile uint32_t g_spi_bench_bad[8];
volatile uint32_t g_spi_bench_read_index;

static uint32_t bench_pattern(uint32_t addr, uint32_t word_index)
{
    return ((addr >> 2) + word_index) * 0x9e3779b9U ^ 0xa5a5a5a5U;
}

static void bench_fill(uint8_t *buf, uint32_t addr, uint32_t len)
{
    uint32_t i;
    uint32_t words = len / 4U;

    for (i = 0U; i < words; i++)
    {
        uint32_t value = bench_pattern(addr, i);

        memcpy(buf + (i * 4U), &value, sizeof(value));
    }
}

static uint32_t bench_verify(const uint8_t *buf, uint32_t addr, uint32_t len)
{
    uint32_t i;
    uint32_t words = len / 4U;
    uint32_t bad = 0U;

    for (i = 0U; i < words; i++)
    {
        uint32_t value;

        memcpy(&value, buf + (i * 4U), sizeof(value));
        if (value != bench_pattern(addr, i))
        {
            bad++;
        }
    }

    return bad;
}

static void bench_print_rate(const char *name, uint32_t bytes,
                             uint32_t cycles, uint32_t bad)
{
    uint64_t kib_per_sec;
    uint32_t kib;

    if (cycles == 0U)
    {
        printf("%-13s timer unavailable, mismatches=%lu\r\n", name,
               (unsigned long)bad);
        return;
    }

    kib_per_sec = ((uint64_t)(bytes / 1024U) * BENCH_CORE_HZ) / cycles;
    kib = (uint32_t)kib_per_sec;
    if (kib < 1024U)
    {
        printf("%-13s %u KiB/s  %u us  bad=%u\r\n", name, (unsigned)kib,
               (unsigned)((cycles + (BENCH_CORE_HZ / 1000000U) - 1U) /
                          (BENCH_CORE_HZ / 1000000U)),
               (unsigned)bad);
    }
    else
    {
        printf("%-13s %u.%u MiB/s  %u us  bad=%u\r\n", name,
               (unsigned)(kib / 1024U),
               (unsigned)(((kib % 1024U) * 10U) / 1024U),
               (unsigned)((cycles + (BENCH_CORE_HZ / 1000000U) - 1U) /
                          (BENCH_CORE_HZ / 1000000U)),
               (unsigned)bad);
    }
}

static void bench_read_mode(const char *name, uint32_t mode, uint32_t clkdiv)
{
    uint32_t offset;
    uint32_t cycles;
    uint32_t bad = 0U;
    uint32_t start;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    start = DWT->CYCCNT;

    for (offset = 0U; offset < BENCH_REGION_BYTES;
         offset += BENCH_BUF_BYTES)
    {
        uint32_t addr = BENCH_REGION_ADDR + offset;

        if (mode == 3U)
        {
            if (!spi_flash_read_quad_io(addr, bench_dst, BENCH_BUF_BYTES,
                                        clkdiv))
            {
                bad = 0xffffffffU;
                break;
            }
        }
        else if (!spi_flash_read_single(addr, bench_dst, BENCH_BUF_BYTES,
                                        (uint8_t)mode, clkdiv))
        {
            bad = 0xffffffffU;
            break;
        }
        bad += bench_verify(bench_dst, addr, BENCH_BUF_BYTES);
    }

    cycles = DWT->CYCCNT - start;
    if (g_spi_bench_read_index < 6U)
    {
        g_spi_bench_cycles[g_spi_bench_read_index] = cycles;
        g_spi_bench_bad[g_spi_bench_read_index] = bad;
        g_spi_bench_read_index++;
    }
    bench_print_rate(name, BENCH_REGION_BYTES, cycles, bad);
}

static void bench_write_blocks(void)
{
    uint32_t sector;
    uint32_t cycles;
    uint32_t bad = 0U;
    uint32_t start;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    start = DWT->CYCCNT;

    for (sector = 0U; sector < BENCH_REGION_BYTES / SPIFLASH_SECTOR_BYTES;
         sector++)
    {
        uint32_t addr = BENCH_REGION_ADDR + (sector * SPIFLASH_SECTOR_BYTES);

        bench_fill(bench_src, addr, SPIFLASH_SECTOR_BYTES);
        if (!SpiFlash_WriteBlocks(bench_src, addr / SPIFLASH_BLOCK_BYTES,
                                  SPIFLASH_SECTOR_BYTES / SPIFLASH_BLOCK_BYTES))
        {
            printf("write-blocks  call failed at sector %lu\r\n",
                   (unsigned long)sector);
            return;
        }
        if (!spi_flash_read_single(addr, bench_dst, SPIFLASH_SECTOR_BYTES,
                                   CMD_READ_DATA, XSPI_CMD_CLKDIV))
        {
            printf("write-blocks  verify read failed at sector %lu\r\n",
                   (unsigned long)sector);
            return;
        }
        bad += bench_verify(bench_dst, addr, SPIFLASH_SECTOR_BYTES);
    }

    cycles = DWT->CYCCNT - start;
    g_spi_bench_cycles[6] = cycles;
    g_spi_bench_bad[6] = bad;
    bench_print_rate("write-4K", BENCH_REGION_BYTES, cycles, bad);
}

static void bench_write_blocks32(void)
{
    uint32_t offset;
    uint32_t cycles;
    uint32_t bad = 0U;
    uint32_t start;
    uint8_t *write_src = USBD1_MSC_BOT_Data;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    start = DWT->CYCCNT;

    for (offset = 0U; offset < BENCH_REGION_BYTES;
         offset += BENCH_WRITE32_BYTES)
    {
        uint32_t addr = BENCH_REGION_ADDR + offset;
        uint32_t part;

        for (part = 0U; part < BENCH_WRITE32_BYTES;
             part += SPIFLASH_SECTOR_BYTES)
        {
            bench_fill(write_src + part, addr + part,
                       SPIFLASH_SECTOR_BYTES);
        }

        if (!SpiFlash_WriteBlocks(write_src,
                                  addr / SPIFLASH_BLOCK_BYTES,
                                  BENCH_WRITE32_BYTES /
                                      SPIFLASH_BLOCK_BYTES))
        {
            printf("write-32K     call failed at 0x%06lX\r\n",
                   (unsigned long)addr);
            return;
        }

        for (part = 0U; part < BENCH_WRITE32_BYTES;
             part += SPIFLASH_SECTOR_BYTES)
        {
            if (!spi_flash_read_single(addr + part, bench_dst,
                                       SPIFLASH_SECTOR_BYTES, CMD_READ_DATA,
                                       XSPI_CMD_CLKDIV))
            {
                printf("write-32K     verify read failed at 0x%06lX\r\n",
                       (unsigned long)(addr + part));
                return;
            }
            bad += bench_verify(bench_dst, addr + part,
                                SPIFLASH_SECTOR_BYTES);
        }
    }

    cycles = DWT->CYCCNT - start;
    g_spi_bench_cycles[7] = cycles;
    g_spi_bench_bad[7] = bad;
    bench_print_rate("write-32K", BENCH_REGION_BYTES, cycles, bad);
}


void SpiFlash_BenchmarkRun(void)
{
    uint32_t sr1 = spi_flash_status();
    uint8_t sr2 = 0U;
    uint8_t sr2_ok = spi_flash_read_status2(&sr2) ? 1U : 0U;
    uint8_t wel = 0U;

    printf("\r\nSPI flash benchmark: region=0x%06X size=%u KiB\r\n",
           (unsigned)BENCH_REGION_ADDR, (unsigned)(BENCH_REGION_BYTES / 1024U));
    printf("status: SR1=0x%02X SR2=0x%02X/ok=%u\r\n",
           (unsigned)(sr1 & 0xffU), (unsigned)sr2, (unsigned)sr2_ok);

    if (spi_flash_write_enable())
    {
        uint32_t s = spi_flash_status();

        wel = (uint8_t)((s >> 1) & 1U);
    }
    printf("write-enable: WEL=%u\r\n", (unsigned)wel);

    /* Use the same block-write path that USB MSC exercises; this both seeds
     * the region and gives a write number before the read-mode comparison. */
    bench_write_blocks();
    bench_write_blocks32();
    bench_read_mode("0x03 div20", CMD_READ_DATA, 20U);
    bench_read_mode("0xEB div16", 3U, 16U);
    bench_read_mode("0xEB div8", 3U, 8U);
    bench_read_mode("0xEB div4", 3U, 4U);
    bench_read_mode("0xEB div2", 3U, 2U);
    printf("SPI flash benchmark: DONE\r\n");
}

#endif /* SPIFLASH_BENCH */
