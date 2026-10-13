// menu.h -- cartridge selection screen
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdbool.h>

// Scans COLECO_DIR for *.ROM. Returns the count, or -1 if the directory is
// missing.
int  menu_scan_roms(void);

// Blocks until the user picks a ROM, writing the full path (e.g.
// "coleco/Donkey Kong.ROM") into out_path and returning true. Returns false if
// the user held Button 1 for a second to leave ColecoJam altogether; the
// caller then hands off to the bootloader.
bool menu_select_rom(char *out_path, size_t out_len);

void menu_message(const char *title, const char *line1, const char *line2,
                  bool is_error);

// Draw one line of text at a character row, clearing that row first. Used for
// the in-game debug overlay, which lives in the border around the 256x192
// ColecoVision image and so never disturbs the picture.
void menu_debug_line(int row, const char *text);

// Boot title sequence: the logo fades up in the centre, a light sweeps across
// it, it holds, then fades out. Any button abandons it. Does nothing when the
// SD card is mounted as a USB drive, and is compiled out when TITLE_SEQUENCE
// is 0. Call before menu_select_rom(), which then fades the menu up.
void menu_title_sequence(void);

// Boot options menu, shown when a cartridge is fitted and Button 1 is held
// during start-up. Returns one of these; BOOT_OPT_LOAD is the default.
enum {
    BOOT_OPT_LOAD = 0,
    BOOT_OPT_COPY = 1,
    BOOT_OPT_USB  = 2,
};
int menu_boot_options(void);

// Hold a message until any button is pressed and released.
void menu_wait_any_input(const char *title, const char *l1, const char *l2);
