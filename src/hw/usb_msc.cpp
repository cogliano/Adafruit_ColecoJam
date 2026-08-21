// usb_msc.cpp -- TinyUSB MSC device: the microSD card appears as a USB drive
// on the native USB-C port, so ROMs can be dragged and dropped from a PC.
//
// The host and the emulator must not touch the filesystem at the same time.
// The rule enforced here is simple: while a host has the volume mounted, the
// menu refuses to browse and asks the user to eject. Writes from the host set
// a dirty flag so the ROM list is rescanned after ejection.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_msc.h"
#include "sdcard.h"
#include "config.h"

#include "tusb.h"
#include <string.h>

static volatile bool host_mounted = false;
static volatile bool media_dirty  = false;
static bool          ejected      = false;
static bool          msc_enabled  = true;

void usb_msc_set_enabled(bool enabled) { msc_enabled = enabled; }
bool usb_msc_enabled(void)             { return msc_enabled; }

void usb_msc_init(void) {
    // Skipping tud_init() entirely means the USB-C port never enumerates, so
    // the host sees no device at all rather than an empty or unreadable drive.
    if (!msc_enabled) return;
    tud_init(BOARD_TUD_RHPORT);
}

void usb_msc_task(void) {
    // tud_task() must not be called when the stack was never initialised.
    if (!msc_enabled) return;
    tud_task();
}

bool usb_msc_host_connected(void) {
    return msc_enabled && host_mounted && !ejected;
}

bool usb_msc_media_dirty(void) {
    bool d = media_dirty;
    media_dirty = false;
    return d;
}

// ---------------------------------------------------------------------------
// TinyUSB device callbacks
// ---------------------------------------------------------------------------
extern "C" {

void tud_mount_cb(void)   { host_mounted = true;  ejected = false; }
void tud_umount_cb(void)  { host_mounted = false; }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; host_mounted = false; }
void tud_resume_cb(void)  { host_mounted = true; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,   "Adafruit", 8);
    memcpy(product_id,  "ColecoJam       ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // no media
        return false;
    }
    return sd_ready();
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
    (void)lun;
    *block_count = sd_block_count();
    *block_size  = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start) ejected = true;
    if (start) ejected = false;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (offset != 0 || (bufsize % 512) != 0) return -1;
    uint32_t blocks = bufsize / 512;
    if (!sd_read_blocks(lba, (uint8_t *)buffer, blocks)) return -1;
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (offset != 0 || (bufsize % 512) != 0) return -1;
    uint32_t blocks = bufsize / 512;
    if (!sd_write_blocks(lba, buffer, blocks)) return -1;
    media_dirty = true;
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

} // extern "C"
