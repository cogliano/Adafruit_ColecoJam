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
#include "logo.h"
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

// Copy the logo into the framebuffer at pixel position (x, y), clipped to the
// screen. Straight RGB565 copy; the image carries its own black background.
static void draw_logo(int x, int y) {
    for (int r = 0; r < LOGO_H; r++) {
        const int fy = y + r;
        if (fy < 0 || fy >= FB_HEIGHT) continue;
        for (int c = 0; c < LOGO_W; c++) {
            const int fx = x + c;
            if (fx < 0 || fx >= FB_WIDTH) continue;
            video_set_pixel(fx, fy, logo_rgb565[r * LOGO_W + c]);
        }
    }
}

// Where the logo sits on the menu. The image carries a column of black padding
// before its first lit pixel, so it is drawn one pixel left of the text column
// to make the letters line up with the header beneath.
#define LOGO_MENU_X   (1 * CHAR_W - LOGO_INK_X)
#define LOGO_MENU_Y   0

#if MENU_SAVER_TIMEOUT_S > 0
// Bounce the logo around the screen, position derived from elapsed time.
//
// Computed from the clock rather than stepped each frame: the menu loop does
// not run at a fixed rate, so accumulating a per-iteration delta would make
// the speed depend on how busy the loop happens to be, and rounding would
// drift over the minutes this runs for. A triangle wave over elapsed time is
// exact and needs no state.
//
// Integer arithmetic throughout, in milliseconds. half_y is the time for one
// top-to-bottom sweep; the horizontal half-period is scaled from it so both
// axes move at the same pixels per second.
static void saver_position(uint32_t elapsed_ms, int *out_x, int *out_y) {
    const int range_y = FB_HEIGHT - LOGO_H;
    const int range_x = FB_WIDTH  - LOGO_W;

    const uint32_t half_y = (uint32_t)MENU_SAVER_BOUNCE_S * 1000u;
    // half_x / half_y == range_x / range_y, so the speeds match.
    const uint32_t half_x = range_x > 0
        ? (uint32_t)((uint64_t)half_y * (uint32_t)range_x / (uint32_t)range_y)
        : 1u;

    // Start where the logo already is on the menu, so switching to the saver
    // looks like the rest of the screen falling away rather than the logo
    // jumping to a corner.
    //
    // Each axis needs its own phase offset: one time shift would move both
    // together, and the menu position does not lie on the diagonal the logo
    // would otherwise be travelling along.
    int start_x = LOGO_MENU_X, start_y = LOGO_MENU_Y;
    if (start_x < 0) start_x = 0;
    if (start_x > range_x) start_x = range_x;
    if (start_y < 0) start_y = 0;
    if (start_y > range_y) start_y = range_y;

    // Rounded UP. The position is recovered from the offset by this same
    // division in reverse, and truncating both ways loses a pixel -- the logo
    // would start one pixel left of where the menu drew it and visibly twitch
    // at the changeover.
    const uint32_t off_x = range_x > 0
        ? (uint32_t)(((uint64_t)start_x * half_x + (uint32_t)range_x - 1u)
                     / (uint32_t)range_x) : 0u;
    const uint32_t off_y = range_y > 0
        ? (uint32_t)(((uint64_t)start_y * half_y + (uint32_t)range_y - 1u)
                     / (uint32_t)range_y) : 0u;

    // Triangle wave: sweep one way over a half period, back over the next.
    const uint32_t py = (elapsed_ms + off_y) % (half_y * 2u);
    const uint32_t px = (elapsed_ms + off_x) % (half_x * 2u);
    int y = (int)((uint64_t)(py % half_y) * (uint32_t)range_y / half_y);
    int x = (int)((uint64_t)(px % half_x) * (uint32_t)range_x / half_x);
    if (py >= half_y) y = range_y - y;
    if (px >= half_x) x = range_x - x;

    *out_x = x;
    *out_y = y;
}

// Number of steps the fade is divided into. Each step scales the framebuffer
// once, so this trades smoothness against work per frame; 32 is imperceptibly
// smooth at half a second and costs about 3 ms a step.
#define SAVER_FADE_STEPS 32

// Darken everything except the logo by one step of a linear fade.
//
// Scaling in place, with no copy of the original: there is no room for a
// second 150 KB framebuffer. To land on original*(1 - i/N) at step i, given
// the buffer already holds original*(1 - (i-1)/N), the factor for this step is
// (N-i)/(N-i+1) -- which is exact and reaches zero on the final step.
//
// Rows are split around the logo rather than testing every pixel, so the inner
// loops stay tight.
static void saver_fade_step(int step, int logo_x, int logo_y) {
    const int rem = SAVER_FADE_STEPS - step;         // numerator
    const int den = rem + 1;
    uint16_t *fb = video_framebuffer();

    for (int y = 0; y < FB_HEIGHT; y++) {
        const bool spans_logo = (y >= logo_y && y < logo_y + LOGO_H);
        int x = 0;
        while (x < FB_WIDTH) {
            int run_end = FB_WIDTH;
            if (spans_logo) {
                if (x < logo_x)               run_end = logo_x;
                else if (x < logo_x + LOGO_W) { x = logo_x + LOGO_W; continue; }
            }
            uint16_t *p = &fb[y * FB_WIDTH + x];
            for (int i = 0; i < run_end - x; i++) {
                const uint16_t c = p[i];
                const uint32_t r = ((c >> 11) & 0x1F) * rem / den;
                const uint32_t g = ((c >>  5) & 0x3F) * rem / den;
                const uint32_t b = ( c        & 0x1F) * rem / den;
                p[i] = (uint16_t)((r << 11) | (g << 5) | b);
            }
            x = run_end;
        }
    }
}

// Paint everything except the logo black. Used to finish the fade, since
// integer rounding can leave a pixel or two just above zero.
static void saver_clear_around_logo(int x, int y) {
    if (y > 0)
        video_fill_rect(0, 0, FB_WIDTH, y, RGB565(0, 0, 0));
    if (y + LOGO_H < FB_HEIGHT)
        video_fill_rect(0, y + LOGO_H, FB_WIDTH, FB_HEIGHT - (y + LOGO_H),
                        RGB565(0, 0, 0));
    if (x > 0)
        video_fill_rect(0, y, x, LOGO_H, RGB565(0, 0, 0));
    if (x + LOGO_W < FB_WIDTH)
        video_fill_rect(x + LOGO_W, y, FB_WIDTH - (x + LOGO_W), LOGO_H,
                        RGB565(0, 0, 0));
}

// Draw one frame of the screen saver.
//
// There is a single framebuffer and the display scans it continuously, so a
// clear-then-redraw leaves a window in which the scanout can read a screen
// with no logo on it. That showed up as the logo flickering roughly once a
// second -- the beat between the redraw rate and the 60 Hz refresh.
//
// Instead the logo is drawn at its NEW position first, and only then is the
// part of the OLD rectangle it no longer covers painted out. The logo is
// therefore present in the framebuffer at every instant, and the erase touches
// a sliver about one pixel wide rather than the whole screen.
static void draw_saver(uint32_t elapsed_ms, bool first_frame) {
    static int ox = 0, oy = 0;

    int x, y;
    saver_position(elapsed_ms, &x, &y);

    if (first_frame) {
        // The fade stage has already blacked out everything but the logo, and
        // the logo is sitting at exactly this position, so there is nothing to
        // draw -- just adopt it as the starting point.
        ox = x; oy = y;
        return;
    }

    if (x == ox && y == oy) return;          // nothing moved; leave it alone

    draw_logo(x, y);

    // Paint out the old rectangle minus the new one: up to four strips, none
    // of which overlaps the logo just drawn.
    const int ix0 = (ox > x) ? ox : x;                    // intersection
    const int ix1 = ((ox + LOGO_W) < (x + LOGO_W)) ? (ox + LOGO_W) : (x + LOGO_W);
    const int iy0 = (oy > y) ? oy : y;
    const int iy1 = ((oy + LOGO_H) < (y + LOGO_H)) ? (oy + LOGO_H) : (y + LOGO_H);

    if (ix0 >= ix1 || iy0 >= iy1) {
        // No overlap at all -- erase the whole of the old position.
        video_fill_rect(ox, oy, LOGO_W, LOGO_H, RGB565(0, 0, 0));
    } else {
        if (iy0 > oy)
            video_fill_rect(ox, oy, LOGO_W, iy0 - oy, RGB565(0, 0, 0));
        if (iy1 < oy + LOGO_H)
            video_fill_rect(ox, iy1, LOGO_W, (oy + LOGO_H) - iy1, RGB565(0, 0, 0));
        if (ix0 > ox)
            video_fill_rect(ox, iy0, ix0 - ox, iy1 - iy0, RGB565(0, 0, 0));
        if (ix1 < ox + LOGO_W)
            video_fill_rect(ix1, iy0, (ox + LOGO_W) - ix1, iy1 - iy0, RGB565(0, 0, 0));
    }

    ox = x; oy = y;
}
#endif

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

    // Title band: the logo, left-aligned with the list header, and the build
    // ID on the right. The band is LOGO_H tall -- three text rows -- so the
    // header on row 3 sits directly beneath it.
    //
    // The band is black rather than the blue used elsewhere because the logo
    // was drawn on black -- its bevels and the insides of its letters are all
    // black-backed, so on any other colour it would sit in a black box.
    video_fill_rect(0, 0, FB_WIDTH, LOGO_H, RGB565(0, 0, 0));
    // Positioned so the logo's letters -- not the edge of the image, which has
    // a column of black padding -- start at the same x as the "SELECT GAME
    // CARTRIDGE" header, which is drawn at text column 1.
    draw_logo(LOGO_MENU_X, LOGO_MENU_Y);
    {
        // The build ID stays on screen so a stale flash is obvious at a
        // glance. Right-justified, and centred vertically in the 16-pixel band.
        const char *b = ACJ_BUILD_ID;
        const int x = FB_WIDTH - (int)strlen(b) * CHAR_W - 4;
        const int y = (LOGO_H - CHAR_H) / 2;
        for (int i = 0; b[i]; i++)
            draw_char(x + i * CHAR_W, y, b[i], COL_DIM, RGB565(0, 0, 0), false);
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
        draw_text(1, 3, "SELECT GAME CARTRIDGE", COL_ACCENT, COL_BG, false);
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
                // hid_app.cpp understood it. These are the MENU_* bits from
                // usb_host.h, not the raw report. Expected values:
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
#if MENU_SAVER_TIMEOUT_S > 0
    // Screen saver state. last_input is bumped by any pad or board button
    // activity; saver_since marks when the bouncing started, so the animation
    // begins from the top-left corner every time rather than mid-flight. The
    // redraw is paced by the vertical blank, so no frame timer is needed.
    absolute_time_t last_input  = get_absolute_time();
    absolute_time_t saver_since = last_input;
    bool            saver       = false;
    int             saver_steps = 0;      // fade steps applied so far
    bool            saver_begun = false;  // has the bounce started?

    // Set when the saver is dismissed, cleared when every button is released.
    // While set, input is ignored entirely -- see where it is applied below.
    bool            swallow_input = false;
#endif

    // Remembered across calls, so returning from a game with Button 1 lands on
    // the cartridge you were just playing rather than the top of the list.
    // Clamped because the list is rescanned each time and may have shrunk.
    static int selected = 0;
    static int scroll   = 0;
    if (selected >= roms.count) selected = roms.count > 0 ? roms.count - 1 : 0;
    if (scroll > selected)      scroll   = selected;
    if (selected >= scroll + LIST_ROWS) scroll = selected - LIST_ROWS + 1;
    if (scroll < 0)             scroll   = 0;
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

        // Repaint when the USB picture changes, so a controller plugged in --
        // or enumerating a moment after the menu is first drawn -- is reported
        // immediately rather than at the next cursor movement.
        //
        // Event-driven rather than on a timer: the counts only move when a
        // device actually arrives or leaves, so this costs one comparison per
        // pass and repaints exactly when there is something new to say.
        {
            static int  last_pads = -1, last_dev = -1, last_hid = -1;
            static int  last_step = -1, last_kb = -1;
            const int   now_pads = usb_host_pad_count();
            const int   now_dev  = usb_host_dev_seen();
            const int   now_hid  = usb_host_hid_seen();
            const int   now_step = usb_host_init_step();
            const int   now_kb   = usb_host_keyboard_connected() ? 1 : 0;
            if (now_pads != last_pads || now_dev != last_dev ||
                now_hid  != last_hid  || now_step != last_step ||
                now_kb   != last_kb) {
                last_pads = now_pads;
                last_dev  = now_dev;
                last_hid  = now_hid;
                last_step = now_step;
                last_kb   = now_kb;
                need_redraw = true;
            }
        }

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
#if MENU_SAVER_TIMEOUT_S > 0
            // The drive screen is its own display; hold the saver off, and do
            // not let the idle time accumulate while it is showing.
            last_input = get_absolute_time();
            saver = false;
#endif
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

#if MENU_SAVER_TIMEOUT_S > 0
        {
            const absolute_time_t t = get_absolute_time();

            // Button 1 is not part of `now` -- it is read separately below --
            // but it must still count as activity, or the saver could not be
            // woken by it and the hold-to-exit would be unreachable while it
            // was running.
            const bool b1 = !gpio_get(PIN_BUTTON1);

            if (now != 0 || b1) {
                last_input = t;
                if (saver) {
                    saver       = false;
                    need_redraw = true;
                    if (now != 0) {
                        // Woken by the pad or Button 2/3: ignore that input
                        // until it is released, so it does not also move the
                        // cursor or load a game.
                        //
                        // Setting rpt.prev alone is not enough. That blocks
                        // the press EDGE, but rpt.next is still at whatever
                        // value it held minutes ago, so edge_or_repeat() takes
                        // its auto-repeat branch on the very next pass and
                        // again 60 ms later -- which moved the cursor two
                        // places on a single press.
                        swallow_input = true;
                        continue;
                    }
                    // Woken by Button 1: fall through, so a press that is held
                    // goes on to count towards leaving ColecoJam.
                }
            } else if (!saver &&
                       absolute_time_diff_us(last_input, t) >=
                           (int64_t)MENU_SAVER_TIMEOUT_S * 1000000) {
                saver       = true;
                saver_since = t;
                saver_steps = 0;
                saver_begun = false;
                // Nothing is drawn yet: the menu stays on screen and the fade
                // below takes it away.
            }

            if (saver) {
                // Wait for the vertical blank, then draw. There is one
                // framebuffer and the display scans it continuously, so
                // starting the update just after a frame ends gives the whole
                // frame period to finish it, and the update rate is locked to
                // the refresh instead of beating against it.
                //
                // Polled rather than video_wait_vsync(), so the USB stacks
                // keep being serviced while we wait. Blocking for a whole
                // frame would drop tuh_task() from the loop's usual rate to
                // 60 Hz for as long as the saver runs, and a host stack that
                // is serviced sparsely is the likeliest way to lose a
                // controller.
                {
                    const uint32_t frame = video_frames_rendered();
                    while (video_frames_rendered() == frame) {
                        usb_host_task();
                        usb_msc_task();
                        tight_loop_contents();
                    }
                }

                const uint32_t since = (uint32_t)
                    (absolute_time_diff_us(saver_since, get_absolute_time())
                     / 1000);

                // Three stages: fade the menu away around the logo, hold
                // everything still, then start the logo moving.
                //
                // Through local constants rather than the macros directly, so
                // setting either to 0 to skip a stage does not produce a
                // "comparison of unsigned expression is always false" warning.
                const uint32_t fade_ms  = (uint32_t)MENU_SAVER_FADE_MS;
                const uint32_t pause_ms = (uint32_t)MENU_SAVER_PAUSE_MS;

                if (since < fade_ms) {
                    // Catch up to the step this moment calls for, so the fade
                    // takes the configured time whatever the frame rate does.
                    const int want = (int)((uint64_t)since * SAVER_FADE_STEPS
                                           / (fade_ms ? fade_ms : 1u));
                    while (saver_steps < want && saver_steps < SAVER_FADE_STEPS) {
                        saver_steps++;
                        saver_fade_step(saver_steps, LOGO_MENU_X, LOGO_MENU_Y);
                    }
                } else if (since < fade_ms + pause_ms) {
                    if (saver_steps < SAVER_FADE_STEPS) {
                        // Finish the fade exactly, then hold. Integer rounding
                        // can leave a pixel just above zero, so this paints the
                        // remainder out rather than trusting the arithmetic.
                        saver_steps = SAVER_FADE_STEPS;
                        saver_clear_around_logo(LOGO_MENU_X, LOGO_MENU_Y);
                    }
                } else {
                    // Bounce. Time is measured from the end of the pause, so
                    // the logo sets off from where the menu drew it.
                    if (!saver_begun) {
                        saver_begun = true;
                        saver_clear_around_logo(LOGO_MENU_X, LOGO_MENU_Y);
                        draw_saver(0, true);
                    } else {
                        draw_saver(since - fade_ms - pause_ms, false);
                    }
                }
                continue;
            }
        }
#endif

        // Drop input left over from dismissing the screen saver, until every
        // button has been released. Placed after the saver block so the held
        // button still counts as activity there and cannot let the saver
        // restart underneath it.
        if (swallow_input) {
            if (now == 0) {
                swallow_input = false;
            } else {
                now = 0;                 // as though nothing were pressed
                rpt.prev = 0;            // so the next real press is an edge
            }
        }

        // Button 1 HELD for a second: leave ColecoJam altogether. Returns false,
        // and main() hands off to coleco_exit() -- the pico-bootLoader picker
        // when we were launched from it, otherwise the UF2 bootloader.
        //
        // This lives here rather than in the emulator because a PRESS of
        // Button 1 during a game now means "back to this menu". Measured from
        // the start of each press, so a press left over from leaving a game
        // (main waits for release first anyway) can never count towards it.
        {
            static absolute_time_t b1_down;
            static bool            b1_held = false;
            if (!gpio_get(PIN_BUTTON1)) {
                if (!b1_held) { b1_held = true; b1_down = get_absolute_time(); }
                else if (absolute_time_diff_us(b1_down, get_absolute_time())
                         >= 1000000) {
                    b1_held = false;
                    return false;
                }
            } else {
                b1_held = false;
            }
        }

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
            const bool kb = usb_host_keyboard_connected();

            if (pads > 0 || kb) {

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
                // Name what is actually attached rather than just counting it.
                // hid_app.cpp recognises most pads by VID/PID ("DS4", "X360",
                // "MSNES", ...) and leaves "??" for the ones it does not, which
                // is the difference between "my pad is unsupported" and "my pad
                // is not plugged in".
                {
                    int o = 0;
                    for (int i = 0; i < 2; i++) {
                        const char *n = usb_host_pad_name(i);
                        if (!n) continue;
                        o += snprintf(st + o, sizeof(st) - o, "%sP%d %s",
                                      o ? "  " : "", i + 1, n);
                    }
                    if (kb)
                        o += snprintf(st + o, sizeof(st) - o, "%sKB",
                                      o ? "  " : "");
                    if (!o)
                        snprintf(st, sizeof(st), "%d controller%s connected",
                                 pads, pads == 1 ? "" : "s");
                }
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
                snprintf(st, sizeof(st), "USB: %d device%s, %d HID, 0 pads, no KB",
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

// ---------------------------------------------------------------------------
// Debug overlay
// ---------------------------------------------------------------------------
// The emulated image is 256x192 centred in a 320x240 framebuffer, leaving a
// 32-pixel side border and 24 pixels top and bottom. Rows 27-29 are entirely
// outside the picture, so text there costs the game nothing.
void menu_debug_line(int row, const char *text) {
    if (row < 0 || row >= ROWS) return;
    video_fill_rect(0, row * CHAR_H, FB_WIDTH, CHAR_H, RGB565(0, 0, 0));
    draw_text(0, row, text, COL_DIM, COL_BG, false);
}
