// bootloader.c -- pico-bootLoader interoperation
//
// Two unrelated jobs, both about being launchable from the resident
// pico-bootLoader (https://github.com/fhoedemakers/pico-bootLoader):
//
//   1. a program-name record the loader can actually read, and
//   2. the watchdog handshake that gets the user back to its picker.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bootloader.h"

#include "pico/binary_info.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/stdlib.h"

// ---------------------------------------------------------------------------
// Program name, in flash
// ---------------------------------------------------------------------------
// The bootloader identifies applications by the binary_info program-name string
// it reads straight out of the .uf2 on the SD card, and matches it against a row
// in /emu/emulators.txt. It resolves the pointers it finds there to UF2 blocks
// by flash target_addr.
//
// That is a problem for us specifically. We link copy_to_ram (see the tail of
// ../../CMakeLists.txt), and memmap_copy_to_ram.ld keeps only .flashdata* in
// flash -- the rest of .rodata is pulled into .data at a RAM address. The SDK's
// own record (standard_binary_info.c) is an ordinary `static const` struct
// pointing at an ordinary string literal, so under copy_to_ram both land around
// 0x2000xxxx. No UF2 block covers those addresses, the loader finds nothing, the
// emulators.txt match fails, and the entry never appears in the picker -- even
// though the image itself boots perfectly. The SDK emits an address-mapping
// table for exactly this case; the bootloader does not use it.
//
// So pin both halves into flash by hand. __in_flash() names a .flashdata.<group>
// section, which section_default_rodata.incl and section_copy_to_ram_rodata.incl
// both place in .rodata > FLASH -- so this is equally correct for the standalone
// build and needs no #if. PICO_NO_BI_PROGRAM_NAME (set in CMakeLists.txt)
// suppresses the SDK's RAM-resident duplicate so there is exactly one record.
//
// Note both halves matter: the loader skips an unreadable descriptor and keeps
// looking, but an unreadable *string* makes it give up on the file entirely.
static const char __in_flash("bootid") coleco_program_name[] = PICO_PROGRAM_NAME;
bi_decl_with_attr(bi_program_name(coleco_program_name), __in_flash("bootid"))

// ---------------------------------------------------------------------------
// Return-to-picker handshake
// ---------------------------------------------------------------------------
// Two watchdog scratch registers carry one-direction signals between the
// resident bootloader and the application it launched. Both survive
// watchdog_reboot(0,0,0) -- which only clobbers scratch[4] -- and are cleared by
// a cold reset, so "launched from the bootloader" correctly becomes false again
// after a power cycle or a BOOTSEL flash.
//
//   scratch[6]: bootloader -> app. Set by the loader immediately before it jumps
//               to our reset vector.
//   scratch[7]: app -> bootloader. Set here just before the reset. The loader
//               checks and clears it in its resume path; when present it skips
//               the resume jump and shows the picker instead.
//
// This duplicates the contract in pico_shared/FrensHelpers.cpp, the same way
// pico-doom's src/pico/doom_boot.c does, because we do not link pico_shared.
// The magics and the scratch indices MUST stay identical to that file, or the
// loader's consumeReturnToBootloaderRequest() will not recognise us.
#define LOADER_LAUNCH_MAGIC   0xB007ED01u
#define LOADER_RETURN_MAGIC   0xB007BACEu
#define LOADER_LAUNCH_SCRATCH 6
#define LOADER_RETURN_SCRATCH 7

bool coleco_launched_from_bootloader(void)
{
    return watchdog_hw->scratch[LOADER_LAUNCH_SCRATCH] == LOADER_LAUNCH_MAGIC;
}

void coleco_return_to_bootloader(void)
{
    watchdog_hw->scratch[LOADER_RETURN_SCRATCH] = LOADER_RETURN_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}
