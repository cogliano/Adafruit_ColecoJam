// eeprom24.h -- 24C08 / 24C256 serial EEPROM, as fitted to Activision PCB carts
//
// Games bit-bang I2C through four cartridge hotspots, so this models the bus
// at the level of the two lines rather than at the level of bytes: the clock
// and data lines go in, and the data line is read back.
//
// The two parts differ in more than size. A 24C08 is addressed with ONE byte
// and puts the top two address bits into the device-address byte; a 24C256 is
// addressed with TWO. The chip in use is therefore not a detail that can be
// papered over, and is chosen per game.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define EEPROM_24C08_BYTES    1024u
#define EEPROM_24C256_BYTES  32768u

// Attach a chip of `size` bytes (one of the two above), using `buffer` for its
// contents and clearing it to 0xFF. Passing 0, or a null buffer, detaches --
// which is what a cartridge without one gets.
//
// The storage is the caller's because a 24C256 is 32 KB, and 32 KB of SRAM is
// far too much to spend on something a bit-banged bus touches a few bytes at a
// time. It lives in PSRAM, past the cartridge.
void eeprom_attach(uint32_t size, uint8_t *buffer);
void eeprom_reset(void);          // bus state only; contents are kept

// The two lines. Writes drive them; the read returns the wired-AND of what the
// game is driving and what the chip is driving, which is how an open-drain bus
// behaves and is what makes acknowledgement visible to the game.
void eeprom_set_scl(bool level);
void eeprom_set_sda(bool level);
bool eeprom_get_sda(void);

// Contents, for loading and saving. eeprom_dirty() reports whether anything
// has been written since it was last cleared, so a save file is only rewritten
// when there is something new in it.
uint8_t *eeprom_data(void);
uint32_t eeprom_size(void);
bool     eeprom_dirty(void);
void     eeprom_clear_dirty(void);
