// sdcard.cpp -- SPI-mode microSD driver + FatFs diskio glue
//
// Supports SDSC, SDHC and SDXC. Card init runs at 400 kHz then steps up to
// SD_BAUD_FAST. Multi-block read/write are used so ROM loading is quick.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sdcard.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <string.h>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

// ---------------------------------------------------------------------------
// Low-level SPI
// ---------------------------------------------------------------------------
static bool     card_ready = false;
static int      last_error = 0;      // see sd_last_error()
static bool     card_is_hc = false;      // block-addressed (SDHC/SDXC)
static uint32_t card_blocks = 0;

static inline void cs_low(void)  { gpio_put(PIN_SD_CS, 0); }
static inline void cs_high(void) { gpio_put(PIN_SD_CS, 1); spi_write_blocking(SD_SPI_PORT, (const uint8_t[]){0xFF}, 1); }

static uint8_t xfer(uint8_t v) {
    uint8_t rx;
    spi_write_read_blocking(SD_SPI_PORT, &v, &rx, 1);
    return rx;
}

static void clock_bytes(int n) {
    uint8_t ff = 0xFF;
    for (int i = 0; i < n; i++) spi_write_blocking(SD_SPI_PORT, &ff, 1);
}

static uint8_t wait_ready(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t v;
    do {
        v = xfer(0xFF);
        if (v == 0xFF) return 0xFF;
    } while (!time_reached(deadline));
    return v;
}

// Send a command and return R1.
static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    if (cmd & 0x80) {                        // ACMDn = CMD55 then CMDn
        cmd &= 0x7F;
        uint8_t r = send_cmd(55, 0);
        if (r > 1) return r;
    }

    if (cmd != 12) {                         // CMD12 skips the ready wait
        cs_high();
        cs_low();
        // CMD0 is the exception: before it the card is not in SPI mode yet and
        // does not drive MISO, so waiting for a ready response times out and
        // the command never gets sent at all. Send it regardless.
        if (wait_ready(500) != 0xFF && cmd != 0) {
            last_error = 4;
            return 0xFF;
        }
    }

    uint8_t buf[6];
    buf[0] = (uint8_t)(0x40 | cmd);
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)arg;
    if (cmd == 0)       buf[5] = 0x95;       // valid CRC for CMD0
    else if (cmd == 8)  buf[5] = 0x87;       // valid CRC for CMD8(0x1AA)
    else                buf[5] = 0x01;       // CRC off in SPI mode
    spi_write_blocking(SD_SPI_PORT, buf, 6);

    if (cmd == 12) xfer(0xFF);               // discard the stuff byte

    uint8_t r1;
    int tries = 10;
    do { r1 = xfer(0xFF); } while ((r1 & 0x80) && --tries);
    return r1;
}

static bool read_datablock(uint8_t *dst, uint32_t len) {
    absolute_time_t deadline = make_timeout_time_ms(200);
    uint8_t token;
    do {
        token = xfer(0xFF);
        if (token != 0xFF) break;
    } while (!time_reached(deadline));
    if (token != 0xFE) return false;

    memset(dst, 0xFF, len);
    spi_read_blocking(SD_SPI_PORT, 0xFF, dst, len);
    xfer(0xFF); xfer(0xFF);                  // discard CRC16
    return true;
}

static bool write_datablock(const uint8_t *src, uint8_t token) {
    if (wait_ready(500) != 0xFF) return false;
    xfer(token);
    if (token == 0xFD) return true;          // stop-transmission token

    spi_write_blocking(SD_SPI_PORT, src, 512);
    xfer(0xFF); xfer(0xFF);                  // dummy CRC
    uint8_t resp = xfer(0xFF);
    return (resp & 0x1F) == 0x05;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
int sd_last_error(void) { return last_error; }

bool sd_init(void) {
    card_ready = false;
    last_error = 0;

    spi_init(SD_SPI_PORT, SD_BAUD_INIT);
    gpio_set_function(PIN_SD_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_MISO, GPIO_FUNC_SPI);
    // MISO must be pulled up. The card leaves it high-impedance until it has
    // entered SPI mode, and a floating input reads as noise -- so the very
    // first response looks like a card that is not there.
    gpio_pull_up(PIN_SD_MISO);
    gpio_init(PIN_SD_CS);
    gpio_set_dir(PIN_SD_CS, GPIO_OUT);
    gpio_put(PIN_SD_CS, 1);

    // >=74 clocks with CS high and MOSI high to enter native SPI mode. 16
    // bytes (128 clocks) rather than the bare minimum: some cards want more,
    // and the extra microseconds cost nothing at 400 kHz.
    sleep_ms(10);
    clock_bytes(16);

    cs_low();
    // No valid R1 from GO_IDLE_STATE means nothing is answering on the bus:
    // card absent, not seated, or a wiring/pin problem.
    // Retry CMD0. Cards can need a few attempts to settle into SPI mode after
    // power-up, and a single try makes a slow-but-working card look absent.
    bool idle = false;
    for (int attempt = 0; attempt < 10 && !idle; attempt++) {
        if (send_cmd(0, 0) == 1) { idle = true; break; }
        sleep_ms(20);
        cs_high();
        clock_bytes(2);
        cs_low();
    }
    if (!idle) { last_error = 1; cs_high(); return false; }

    uint8_t ocr[4];
    if (send_cmd(8, 0x1AA) == 1) {
        // SD v2 or later
        for (int i = 0; i < 4; i++) ocr[i] = xfer(0xFF);
        // Card answered CMD8 but echoed the wrong check pattern.
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) { last_error = 2; cs_high(); return false; }

        absolute_time_t deadline = make_timeout_time_ms(1000);
        while (send_cmd(0x80 | 41, 1UL << 30)) {            // ACMD41 HCS=1
            // Card is present and talking but never leaves idle state.
            if (time_reached(deadline)) { last_error = 3; cs_high(); return false; }
        }
        if (send_cmd(58, 0) != 0) { last_error = 3; cs_high(); return false; }  // READ_OCR
        for (int i = 0; i < 4; i++) ocr[i] = xfer(0xFF);
        card_is_hc = (ocr[0] & 0x40) != 0;
    } else {
        // SD v1 (or MMC). Try ACMD41 first, fall back to CMD1.
        uint8_t cmd = (send_cmd(0x80 | 41, 0) <= 1) ? (0x80 | 41) : 1;
        absolute_time_t deadline = make_timeout_time_ms(1000);
        while (send_cmd(cmd, 0)) {
            if (time_reached(deadline)) { last_error = 3; cs_high(); return false; }
        }
        if (send_cmd(16, 512) != 0) { last_error = 3; cs_high(); return false; }
        card_is_hc = false;
    }

    // Capacity from the CSD.
    if (send_cmd(9, 0) == 0) {
        uint8_t csd[16];
        if (read_datablock(csd, 16)) {
            if ((csd[0] >> 6) == 1) {                  // CSD v2
                uint32_t csize = ((uint32_t)(csd[7] & 0x3F) << 16) |
                                 ((uint32_t)csd[8] << 8) | csd[9];
                card_blocks = (csize + 1) * 1024;
            } else {                                   // CSD v1
                uint32_t csize = ((uint32_t)(csd[6] & 0x03) << 10) |
                                 ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
                uint32_t mult  = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
                uint32_t rdblk = csd[5] & 0x0F;
                card_blocks = (csize + 1) * (1UL << (mult + 2)) *
                              ((1UL << rdblk) / 512);
            }
        }
    }

    cs_high();
    spi_set_baudrate(SD_SPI_PORT, SD_BAUD_FAST);
    card_ready = true;
    return true;
}

bool     sd_ready(void)       { return card_ready; }
uint32_t sd_block_count(void) { return card_blocks; }

bool sd_read_blocks(uint32_t lba, uint8_t *dst, uint32_t count) {
    if (!card_ready || count == 0) return false;
    uint32_t addr = card_is_hc ? lba : (lba * 512);

    cs_low();
    bool ok = true;
    if (count == 1) {
        if (send_cmd(17, addr) != 0 || !read_datablock(dst, 512)) ok = false;
    } else {
        if (send_cmd(18, addr) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                if (!read_datablock(dst + i * 512, 512)) { ok = false; break; }
            }
            send_cmd(12, 0);
        } else ok = false;
    }
    cs_high();
    return ok;
}

bool sd_write_blocks(uint32_t lba, const uint8_t *src, uint32_t count) {
    if (!card_ready || count == 0) return false;
    uint32_t addr = card_is_hc ? lba : (lba * 512);

    cs_low();
    bool ok = true;
    if (count == 1) {
        if (send_cmd(24, addr) != 0 || !write_datablock(src, 0xFE)) ok = false;
    } else {
        send_cmd(0x80 | 23, count);                 // ACMD23 pre-erase
        if (send_cmd(25, addr) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                if (!write_datablock(src + i * 512, 0xFC)) { ok = false; break; }
            }
            if (!write_datablock(nullptr, 0xFD)) ok = false;
        } else ok = false;
    }
    if (ok) wait_ready(500);
    cs_high();
    return ok;
}

// ---------------------------------------------------------------------------
// FatFs diskio
// ---------------------------------------------------------------------------
extern "C" {

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv) return STA_NOINIT;
    return card_ready ? (DSTATUS)0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv) return STA_NOINIT;
    if (!card_ready) sd_init();
    return card_ready ? (DSTATUS)0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv) return RES_PARERR;
    return sd_read_blocks((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv) return RES_PARERR;
    return sd_write_blocks((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:         return RES_OK;
    case GET_SECTOR_COUNT:  *(LBA_t *)buff = card_blocks;  return RES_OK;
    case GET_SECTOR_SIZE:   *(WORD *)buff  = 512;          return RES_OK;
    case GET_BLOCK_SIZE:    *(DWORD *)buff = 1;            return RES_OK;
    default:                return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    // No RTC on board; return a fixed 2026-01-01 00:00:00 timestamp.
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

} // extern "C"
