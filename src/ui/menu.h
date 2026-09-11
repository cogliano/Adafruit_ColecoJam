// menu.h -- cartridge selection screen
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdbool.h>

// Scans COLECO_DIR for *.ROM. Returns the count, or -1 if the directory is
// missing.
int  menu_scan_roms(void);

// Blocks until the user picks a ROM. Writes the full path (e.g.
// "coleco/Donkey Kong.ROM") into out_path. Returns false only if the user
// somehow leaves without choosing.
bool menu_select_rom(char *out_path, size_t out_len);

void menu_message(const char *title, const char *line1, const char *line2,
                  bool is_error);

// Draw one line of text at a character row, clearing that row first. Used for
// the in-game debug overlay, which lives in the border around the 256x192
// ColecoVision image and so never disturbs the picture.
void menu_debug_line(int row, const char *text);
