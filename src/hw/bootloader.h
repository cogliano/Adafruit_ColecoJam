// bootloader.h -- pico-bootLoader interoperation
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True when this image was started by the resident pico-bootLoader rather than
// flashed standalone over BOOTSEL. A runtime test, not a build-time one: the
// same binary answers correctly either way.
bool coleco_launched_from_bootloader(void);

// Ask the bootloader to show its picker on the next boot, then reset. Does not
// return. Only meaningful when coleco_launched_from_bootloader() is true; on a
// standalone image the reset just restarts us.
void coleco_return_to_bootloader(void);

#ifdef __cplusplus
}
#endif
