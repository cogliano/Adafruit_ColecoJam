// menu.cpp -- "Select Game Cartridge" browser
//
// Scans coleco/ on the SD card for *.ROM and presents a scrolling list. Layout
// follows the same idea as the menus in fhoedemakers' pico-infonesPlus /
// pico-snesPlus: a title bar, a paged file list with a highlighted row, and a
// status line showing what is connected.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "menu.h"
#include "font8x8.h"
#include "config.h"
#include "../hw/video_hstx.h"
#include "../hw/usb_host.h"
#include "../hw/usb_msc.h"
#include "../hw/audio.h"
#include "../hw/cart_reader.h"

#include "pico/stdlib.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

extern "C" {
#include "ff.h"
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
#define COL_BG        RGB565(  8,  12,  32)
#define COL_TITLE_BG  RGB565( 32,  56, 140)
#define COL_TITLE_FG  RGB565(255, 255, 255)
#define COL_TEXT      RGB565(200, 208, 224)
#define COL_DIM       RGB565(110, 120, 145)
#define COL_SEL_BG    RGB565(220, 170,  40)
#define COL_SEL_FG    RGB565( 16,  16,  24)
#define COL_ACCENT    RGB565( 90, 200, 120)
#define COL_ERROR     RGB565(230,  90,  80)

#define CHAR_W 8
#define CHAR_H 8
#define COLS   (FB_WIDTH / CHAR_W)      // 40
#define ROWS   (FB_HEIGHT / CHAR_H)     // 30

#define LIST_TOP_ROW  5
#define LIST_ROWS     20

#if SHOW_HID_DEBUG
  #define FOOTER_ROWS 4
  #define ROW_HELP    (ROWS - 4)
  #define ROW_STATUS  (ROWS - 3)
#else
  #define FOOTER_ROWS 3
  #define ROW_HELP    (ROWS - 3)
  #define ROW_STATUS  (ROWS - 1)
#endif

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg,
                      bool draw_bg) {
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) c = ' ';
    const uint8_t *g = font8x8[c - FONT_FIRST_CHAR];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))      video_set_pixel(x + col, y + row, fg);
            else if (draw_bg)              video_set_pixel(x + col, y + row, bg);
        }
    }
}

static void draw_text(int col, int row, const char *s, uint16_t fg,
                      uint16_t bg, bool draw_bg) {
    int x = col * CHAR_W, y = row * CHAR_H;
    while (*s && x < FB_WIDTH) {
        draw_char(x, y, *s++, fg, bg, draw_bg);
        x += CHAR_W;
    }
}

static void draw_text_centered(int row, const char *s, uint16_t fg,
                               uint16_t bg, bool draw_bg) {
    int len = (int)strlen(s);
    int col = (COLS - len) / 2;
    if (col < 0) col = 0;
    draw_text(col, row, s, fg, bg, draw_bg);
}

// ---------------------------------------------------------------------------
// ROM list
// ---------------------------------------------------------------------------
struct RomList {
    char     name[MAX_ROM_ENTRIES][MAX_FILENAME_LEN];
    uint32_t size[MAX_ROM_ENTRIES];
    int      count;
};

static RomList roms;

static bool has_rom_extension(const char *name) {
    size_t n = strlen(name);
    size_t e = strlen(ROM_EXTENSION);
    if (n <= e) return false;
    const char *tail = name + n - e;
    for (size_t i = 0; i < e; i++) {
        char a = tail[i], b = ROM_EXTENSION[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return false;
    }
    return true;
}

static void sort_roms(void) {
    // Insertion sort: the list is small and usually near-sorted from the FAT.
    for (int i = 1; i < roms.count; i++) {
        char tmp_name[MAX_FILENAME_LEN];
        uint32_t tmp_size = roms.size[i];
        strcpy(tmp_name, roms.name[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(roms.name[j], tmp_name) > 0) {
            strcpy(roms.name[j + 1], roms.name[j]);
            roms.size[j + 1] = roms.size[j];
            j--;
        }
        strcpy(roms.name[j + 1], tmp_name);
        roms.size[j + 1] = tmp_size;
    }
}

int menu_scan_roms(void) {
    roms.count = 0;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, COLECO_DIR) != FR_OK) return -1;

    while (roms.count < MAX_ROM_ENTRIES) {
        if (f_readdir(&dir, &fno) != FR_OK) break;
        if (fno.fname[0] == 0) break;
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        if (!has_rom_extension(fno.fname)) continue;

        strncpy(roms.name[roms.count], fno.fname, MAX_FILENAME_LEN - 1);
        roms.name[roms.count][MAX_FILENAME_LEN - 1] = 0;
        roms.size[roms.count] = (uint32_t)fno.fsize;
        roms.count++;
    }
    f_closedir(&dir);

    sort_roms();
    return roms.count;
}

// ---------------------------------------------------------------------------
// Screen drawing
// ---------------------------------------------------------------------------
static void draw_frame(int selected, int scroll, const char *status,
                       uint16_t status_col) {
    video_clear(COL_BG);

    // Title bar
    video_fill_rect(0, 0, FB_WIDTH, CHAR_H * 2, COL_TITLE_BG);
    draw_text_centered(0, "SELECT GAME CARTRIDGE", COL_TITLE_FG, COL_TITLE_BG, false);
    {
        // Title line carries the build ID so a stale flash is obvious.
        char sub[48];
        snprintf(sub, sizeof(sub), "Adafruit ColecoJam - %s", ACJ_BUILD_ID);
        draw_text_centered(1, sub, COL_TITLE_FG, COL_TITLE_BG, false);
    }

    if (roms.count == 0) {
        draw_text_centered(11, "No .ROM files found in /coleco", COL_ERROR, COL_BG, false);
        draw_text_centered(13, "Connect USB-C and drop ROMs onto", COL_DIM, COL_BG, false);
        draw_text_centered(14, "the drive, then eject and reset.", COL_DIM, COL_BG, false);
    } else {
        char buf[COLS + 1];
        for (int i = 0; i < LIST_ROWS; i++) {
            int idx = scroll + i;
            if (idx >= roms.count) break;

            int row = LIST_TOP_ROW + i;
            bool sel = (idx == selected);

            if (sel) video_fill_rect(0, row * CHAR_H, FB_WIDTH, CHAR_H, COL_SEL_BG);

            // "> Donkey Kong.ROM ................ 24K"
            char sizebuf[12];
            snprintf(sizebuf, sizeof(sizebuf), "%luK",
                     (unsigned long)((roms.size[idx] + 1023) / 1024));

            int name_room = COLS - 3 - (int)strlen(sizebuf) - 1;
            snprintf(buf, sizeof(buf), "%c %-*.*s %s",
                     sel ? '>' : ' ', name_room, name_room,
                     roms.name[idx], sizebuf);

            draw_text(0, row, buf,
                      sel ? COL_SEL_FG : COL_TEXT,
                      sel ? COL_SEL_BG : COL_BG, false);
        }

        // Scroll position indicator
        char pos[24];
        snprintf(pos, sizeof(pos), "%d / %d", selected + 1, roms.count);
        draw_text(COLS - (int)strlen(pos) - 1, 3, pos, COL_DIM, COL_BG, false);
        draw_text(1, 3, "CARTRIDGES", COL_ACCENT, COL_BG, false);
    }

    // Footer
    // Footer layout. With SHOW_HID_DEBUG the raw report needs two extra rows:
    //
    //   off              on
    //   ROWS-3  help     ROWS-4  help
    //   ROWS-1  status   ROWS-3  status
    //                    ROWS-2  raw report bytes 0..11
    //                    ROWS-1  raw report bytes 12.., or length and count
    //
    // The list ends at LIST_TOP_ROW + LIST_ROWS - 1 = 24, leaving room for
    // four footer rows without overlapping it.
    video_fill_rect(0, (ROWS - FOOTER_ROWS) * CHAR_H,
                    FB_WIDTH, CHAR_H * FOOTER_ROWS, COL_BG);
    // Mention USB drive mode only when it is off, since that is the state
    // someone might be puzzled by ("why is my SD card not showing up?").
    draw_text_centered(ROW_HELP,
                       usb_msc_enabled()
                           ? "D-PAD: browse    A / START: load"
                           : "A/START: load   USB drive: BTN1 at boot",
                       COL_DIM, COL_BG, false);

#if SHOW_HID_DEBUG
    // Raw HID report from pad 0, as hex. Invaluable when a controller decodes
    // wrongly: press each direction and button and read off which byte moves.
    {
        uint8_t raw[24];
        int n = usb_host_raw_report(0, raw, sizeof(raw));
        if (n > 0) {
            // Two rows of 12 bytes. The screen is 40 columns and each byte
            // takes 3, so a single row cannot show a long report -- which is
            // exactly the case where the interesting bytes are at the end.
            char hx[64];
            int o = 0;
            for (int i = 0; i < n && i < 12; i++)
                o += snprintf(hx + o, sizeof(hx) - o, "%02X ", raw[i]);
            draw_text_centered(ROWS - 2, hx, COL_DIM, COL_BG, false);

            if (n > 12) {
                o = 0;
                for (int i = 12; i < n; i++)
                    o += snprintf(hx + o, sizeof(hx) - o, "%02X ", raw[i]);
                draw_text_centered(ROWS - 1, hx, COL_DIM, COL_BG, false);
            } else {
                // Decoded bitmask as well as the raw bytes. The raw dump only
                // proves the controller sent something; this proves how
                // decode_report() understood it. Expected values:
                //   UP 0001  DOWN 0002  LEFT 0004  RIGHT 0008
                //   A  0010  B    0020  X    0040  Y     0080
                //   L  0100  R    0200  SELECT 0400  START 0800
                char ln[48];
                snprintf(ln, sizeof(ln), "len %d  btn %04X  rpt %lu",
                         usb_host_report_len(0),
                         (unsigned)(usb_host_raw_buttons(0) |
                                    usb_host_raw_buttons(1)),
                         (unsigned long)usb_host_report_count());
                draw_text_centered(ROWS - 1, ln, COL_DIM, COL_BG, false);
            }
        }
    }
#endif

    if (status) draw_text_centered(ROW_STATUS, status, status_col, COL_BG, false);
}

// ---------------------------------------------------------------------------
// Input: simple edge detection with auto-repeat
// ---------------------------------------------------------------------------
struct Repeat {
    uint16_t prev;
    absolute_time_t next;
    uint16_t held;
};

static bool edge_or_repeat(Repeat *r, uint16_t now, uint16_t mask) {
    bool pressed_now = (now & mask) != 0;
    bool was_pressed = (r->prev & mask) != 0;

    if (pressed_now && !was_pressed) {
        r->next = make_timeout_time_ms(350);
        return true;
    }
    if (pressed_now && was_pressed && time_reached(r->next)) {
        r->next = make_timeout_time_ms(60);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
bool menu_select_rom(char *out_path, size_t out_len) {
    int selected = 0;
    int scroll   = 0;
    Repeat rpt = { 0, get_absolute_time(), 0 };

    const char *status = nullptr;
    uint16_t status_col = COL_DIM;
    bool need_redraw = true;
    bool was_mounted = false;

    // Slow LED heartbeat, toggled from this loop. If the LED stops changing,
    // core 0 is wedged; if it keeps ticking while the screen is blank, the
    // fault is in video rather than in this loop. Those need different fixes
    // and are otherwise indistinguishable.
    absolute_time_t next_beat = make_timeout_time_ms(500);
    absolute_time_t next_frame_redraw = make_timeout_time_ms(1000);
#if SHOW_HID_DEBUG
    absolute_time_t next_hid_refresh = make_timeout_time_ms(120);
#endif
    bool beat = false;

    for (;;) {
#if !USB_HOST_ON_CORE1
        usb_host_task();
#endif
        usb_msc_task();

#if SHOW_HID_DEBUG
        // Repaint periodically so the raw HID dump is live. Without this the
        // screen only redraws when the selection moves, so buttons that do not
        // move the cursor -- A, B, X, Y, the shoulders, Select, Start -- look
        // like they change nothing, when the display is simply stale. That
        // cost me a debugging round, hence the comment.
        if (usb_host_pad_count() > 0 && time_reached(next_hid_refresh)) {
            next_hid_refresh = make_timeout_time_ms(120);
            need_redraw = true;
        }
#endif

        if (time_reached(next_beat)) {
            beat = !beat;
            gpio_put(PIN_LED, beat ? 0 : 1);
            next_beat = make_timeout_time_ms(500);
        }

        // While a PC has the SD card mounted, block browsing so the two sides
        // never write the FAT at once.
        bool mounted = usb_msc_host_connected();
        if (mounted != was_mounted) {
            was_mounted = mounted;
            need_redraw = true;
        }
        if (mounted) {
            if (need_redraw) {
                video_clear(COL_BG);
                video_fill_rect(0, 0, FB_WIDTH, CHAR_H * 2, COL_TITLE_BG);
                draw_text_centered(0, "USB DRIVE MODE", COL_TITLE_FG, COL_TITLE_BG, false);
                draw_text_centered(11, "SD card is mounted on your computer.", COL_TEXT, COL_BG, false);
                draw_text_centered(13, "Copy .ROM files into /coleco,", COL_DIM, COL_BG, false);
                draw_text_centered(14, "then eject the drive to continue.", COL_DIM, COL_BG, false);
                need_redraw = false;
                next_frame_redraw = make_timeout_time_ms(1000);
            }
            // Repaint once a second. A framebuffer that is still being drawn
            // into but shows nothing means the scanout stopped, not the loop.
            if (time_reached(next_frame_redraw)) {
                need_redraw = true;
                next_frame_redraw = make_timeout_time_ms(1000);
            }
            // Do not spin flat out: every iteration calls into the SD driver
            // on behalf of the host, and starving everything else achieves
            // nothing here.
            sleep_ms(2);
            continue;
        }

        if (usb_msc_media_dirty()) {
            menu_scan_roms();
            if (selected >= roms.count) selected = roms.count ? roms.count - 1 : 0;
            need_redraw = true;
        }

        // Combine both controllers so either can drive the menu.
        uint16_t now = (uint16_t)(usb_host_raw_buttons(0) | usb_host_raw_buttons(1));

        // Board buttons work too, in case no pad is plugged in yet.
        if (!gpio_get(PIN_BUTTON2)) now |= MENU_DOWN;
        if (!gpio_get(PIN_BUTTON3)) now |= MENU_A;

        if (roms.count > 0) {
            if (edge_or_repeat(&rpt, now, MENU_UP)) {
                if (--selected < 0) selected = roms.count - 1;
                need_redraw = true;
            }
            if (edge_or_repeat(&rpt, now, MENU_DOWN)) {
                if (++selected >= roms.count) selected = 0;
                need_redraw = true;
            }
            if (edge_or_repeat(&rpt, now, MENU_LEFT)) {
                selected -= LIST_ROWS;
                if (selected < 0) selected = 0;
                need_redraw = true;
            }
            if (edge_or_repeat(&rpt, now, MENU_RIGHT)) {
                selected += LIST_ROWS;
                if (selected >= roms.count) selected = roms.count - 1;
                need_redraw = true;
            }

            // Edge-detect A and START separately. Testing them as one mask
            // means that if either bit is already set in prev, the other
            // button's press edge is swallowed -- and MENU_A is also driven by
            // board Button 3, so a low reading on GPIO5 would permanently
            // block START.
            const bool a_edge     = (now & MENU_A)     && !(rpt.prev & MENU_A);
            const bool start_edge = (now & MENU_START) && !(rpt.prev & MENU_START);

            if (a_edge || start_edge) {
                snprintf(out_path, out_len, "%s/%s", COLECO_DIR, roms.name[selected]);
                rpt.prev = now;
                return true;
            }
        }

        rpt.prev = now;

        // Keep the highlighted entry inside the visible window.
        if (selected < scroll) { scroll = selected; need_redraw = true; }
        if (selected >= scroll + LIST_ROWS) {
            scroll = selected - LIST_ROWS + 1;
            need_redraw = true;
        }

        if (need_redraw) {
            int pads = usb_host_pad_count();
            int hid  = usb_host_hid_seen();
            int dev  = usb_host_dev_seen();
            // All three are unused when SHOW_CART_DEBUG replaces this line.
            (void)pads; (void)hid; (void)dev;
            static char st[64];
#if SHOW_CART_DEBUG
            // Cartridge probe first, and regardless of controllers: the two
            // bytes seen through each of the four chip selects.
            {
                uint8_t cb[8];
                int n = cart_probe_bytes(cb, sizeof(cb));
                if (n > 0) {
                    int o = snprintf(st, sizeof(st), "cs");
                    for (int i = 0; i < n && o < (int)sizeof(st) - 4; i++)
                        o += snprintf(st + o, sizeof(st) - o, " %02X", cb[i]);
                    status_col = cart_header_bank() >= 0 ? COL_ACCENT : COL_ERROR;
                } else {
                    snprintf(st, sizeof(st), "cart: not probed");
                    status_col = COL_ERROR;
                }
            }
#else
            if (pads > 0) {

#if SHOW_HID_DEBUG
                // Audio plumbing state, kept behind the debug switch:
                //   fl  DAC flag register; 99 = DACs and HP drivers running
                //   r   page 1 0x23, output mixer routing
                //   v   page 1 0x24, analog volume; bit 7 connects the path
                //   b   buffers handed to the I2S DMA
                snprintf(st, sizeof(st), "%dp fl%02X r%02X v%02X b%lu",
                         pads,
                         audio_codec_flags(),
                         audio_route_reg(),
                         audio_hpvol_reg(),
                         (unsigned long)audio_buffers_sent());
#else
                snprintf(st, sizeof(st), "%d controller%s connected",
                         pads, pads == 1 ? "" : "s");
#endif
                status_col = COL_ACCENT;
            } else if (!usb_host_init_done()) {
                // usb_host_init() never returned. The step number says which
                // call on core 1 blocked -- see usb_host.h for the mapping.
                snprintf(st, sizeof(st), "USB host init stuck at step %d of 7",
                         usb_host_init_step());
                status_col = COL_ERROR;
            } else {
                // Report the raw counts. dev counts everything including the
                // CH334F hub, so dev=0 means a dead bus (power or PIO-USB),
                // while dev>0 hid=0 means the hub is up but the pad is not.
                snprintf(st, sizeof(st), "USB: %d device%s, %d HID, 0 pads",
                         dev, dev == 1 ? "" : "s", hid);
                status_col = COL_ERROR;
            }
#endif
            status = st;

            draw_frame(selected, scroll, status, status_col);
            need_redraw = false;
        }

        sleep_ms(5);
    }
}

// ---------------------------------------------------------------------------
// Simple full-screen message, used for boot progress and fatal errors
// ---------------------------------------------------------------------------
void menu_message(const char *title, const char *line1, const char *line2,
                  bool is_error) {
    video_clear(COL_BG);
    video_fill_rect(0, 0, FB_WIDTH, CHAR_H * 2, is_error ? COL_ERROR : COL_TITLE_BG);
    draw_text_centered(0, title, COL_TITLE_FG, COL_BG, false);
    if (line1) draw_text_centered(13, line1, COL_TEXT, COL_BG, false);
    if (line2) draw_text_centered(15, line2, COL_DIM,  COL_BG, false);
}
