# Adafruit_ColecoJam

A ColecoVision emulator for the **Adafruit Fruit Jam** (RP2350B), written in
C++ against the Pico SDK and formatted for Visual Studio Code.

Output goes to the Fruit Jam's HDMI/DVI port over HSTX, sound goes to the
onboard TLV320DAC3100, games load from the microSD card (or straight off a real
cartridge, if you have the reader shield), and control is via one or two USB
gamepads, with an optional USB keyboard for the ColecoVision keypad.

---

## Audio over HDMI

Sound can travel over the HDMI cable instead of the headphone jack. Two
settings in `include/config.h` control it:

```c
#define AUDIO_SINK    AUDIO_SINK_HDMI      // or AUDIO_SINK_CODEC
#define VIDEO_DRIVER  VIDEO_DRIVER_PICO_HDMI   // or VIDEO_DRIVER_HSTX
```

HDMI is the default. Selecting `AUDIO_SINK_HDMI` with the built-in HSTX driver
raises a `#error`, because that driver emits no data islands and the result
would be silent.

This is built on [pico_hdmi](https://github.com/fliperama86/pico_hdmi), the
same library the sister emulator projects use for picture and sound over HDMI.
Carrying audio needs TERC4 symbol encoding, data island periods with guard
bands, BCH error correction per packet, AVI and Audio InfoFrames, and N/CTS
clock regeneration — all of which that library already implements and has
tested on this class of board. Run `tools/fetch_deps.sh` to pull it into
`third_party/pico_hdmi`.

Three things change when it is selected:

- **Core 1 belongs to the library.** `video_output_core1_run()` never returns,
  so the USB host and the emulator both run on core 0. That is safe here
  precisely because core 0 no longer carries a video interrupt for PIO-USB to
  contend with — the reason the host was moved off core 0 originally.
- **clk_sys becomes 252 MHz.** The library derives `clk_hstx` as
  `clk_sys / PICO_HDMI_HSTX_CLK_DIV`, and 640x480p60 needs 126 MHz, so the
  divider is 2. 252 is also 21 x 12 MHz, which is what Pico-PIO-USB requires,
  so both share one PLL and the `pll_usb` retune the built-in driver needs is
  dropped.
- **The codec is not initialised**, freeing its I2C bus and skipping the 350 ms
  de-pop wait.
- **The image runs from RAM** (`copy_to_ram`). Both cores fetch instructions
  through one XIP cache, and the library's core-1 scanline loop has about a 6 us
  deadline per line. Heavy flash-resident work on core 0 -- `sd_init()` and
  FatFs are the first of it during boot -- stalls core 1 on cache misses and
  desyncs the HSTX command stream, killing the picture while everything else
  still reports success. About 90 KB of text joins ~298 KB of statics, well
  inside the 520 KB available.

Switching back to `AUDIO_SINK_CODEC` with `VIDEO_DRIVER_HSTX` restores the
previous arrangement exactly: 240 MHz, `clk_hstx` from a retuned `pll_usb`, and
USB host on core 1.

## MegaCart

`ENABLE_MEGACART` plays cartridges larger than 32 KB, up to the format's 1 MB
maximum.

```
0x8000-0xBFFF   the last bank of the ROM, always fixed
0xC000-0xFFFF   switchable 16 KB bank
```

Note which way round that is: the **fixed** half is the lower one. That is why
a MegaCart carries its `AA 55` header at the start of its *last* bank -- the
last bank is what the BIOS sees at 0x8000.

The bank is chosen by **reading** 0xFFC0 + n, not by writing. Those addresses
sit inside the switchable window itself, so an ordinary instruction fetch there
switches banks as a side effect, which is how the hardware behaves and what
games depend on.

**Every** cartridge is held in the board's 8 MB PSRAM, bank-switched or not.
PSRAM is memory-mapped, so the emulator reads cartridge bytes from it at a
pointer. Only core 0 touches it, and core 1's video code and framebuffer are
both in SRAM, so HDMI timing is unaffected.

Ordinary cartridges used to load into a 32 KB SRAM buffer while only MegaCarts
went to PSRAM. Keeping both spent 32 KB of the scarcest resource on the board
on a copy of something PSRAM holds comfortably, and that 32 KB is what lets the
title chime and the Super Game Module coexist. The cost is that PSRAM is now
required to load anything at all, which is reported plainly at boot if it ever
fails.

Nothing here initialises the chip: the SDK does that during runtime init from
`PICO_PSRAM_SIZE_BYTES` and `PICO_PSRAM_CS_PIN` in the Fruit Jam board header,
before `main()` runs.

**The PSRAM has to be re-timed after the clock change.** The SDK sets the QMI
divider during runtime init, from whatever clk_sys is at that moment, and
`main()` later raises it to 252 MHz for the HDMI output -- which doubles the
PSRAM clock past the chip's 133 MHz limit. The failure is not obvious: most
reads are fine and the occasional one comes back with a bit flipped, so a ROM
loads looking almost right and the machine refuses to start it.
`psram_configure_params()` and `psram_reinitialize()` are called immediately
after the clock is set and before core 1 exists, since re-initialising reaches
into the QMI.

The ROM is read and written through the PSRAM memory window directly, rather
than by reserving a variable in it, which keeps the linker out of it entirely.
The window address is architectural, but it is not taken on trust: the SDK's
`psram_check_address()` is asked about both ends of the range, and a pattern is
written and read back at each end. If any of that fails, MegaCart support
switches itself off with a message rather than reading from somewhere
arbitrary. If that fails, oversized cartridges are refused with a message
rather than loading as corrupt data, which would otherwise look like a bad ROM
dump rather than a hardware fault.

Nothing has to guess which bank boots: the last bank is permanently at 0x8000,
so a cartridge's header is visible from the start whatever the switchable
window holds. The switchable window begins at bank 0 and games set it
themselves.

### Boot menu

Holding **Button 1** during start-up **with a cartridge fitted** shows:

```
SELECT OPTION:
> 1: Load cartridge
  2: Copy cartridge
  3: USB drive mode
```

The build ID sits in the title band and the controller line along the bottom,
both as on the ROM browser. The controller line is rechecked each pass, so a
pad plugged in while this screen is up appears straight away.

Button 2 moves, Button 3 selects; a pad works too once it has enumerated.
Option 1 is the default, and the screen saver runs here as it does on the ROM
browser -- the logo is in the same place on both, so it fades away and sets off
from exactly where it already is. Without a cartridge fitted, Button 1 keeps its old
meaning and goes straight to USB drive mode.

**Copy cartridge** writes the cartridge to `/coleco` as `game.rom`, or
`game2.rom`, `game3.rom` and so on if that name is taken.

The copy screen reports exact byte counts and what the reader made of each
chip select -- `read 24576, saved 20480, banks PPBU`. P is a populated chip, B
a uniform 0xFF/0x00 bank treated as a real but blank chip, U a uniform bank
treated as an empty socket, and `-` a select never reached.

An empty socket is detected two ways, because neither alone is sufficient on
this hardware.

The direct test is whether the bus **follows what was read before it**. An
undriven bus settles to 0xC0, but RP2350-E9 means a pad with a pull-down can
hold the level it was last driven to, so the first bytes of an empty socket are
whatever the previous bank put on the bus -- a dump reading `40 40 C0 C0 C0...`
looks like varying data and gets mistaken for a chip. No ROM returns a
different byte for the same address depending on what preceded it, so each
address is read twice, once after driving the bus to one value and once to
another. A real chip answers the same both times.

The second test is the shape of the data, which catches a socket that has
already settled to a constant 0xC0 by the time it is sampled.

A bank that reads as one repeated value is re-tested before it is believed.
An absent chip is uniform every time under any timing; a bus that has not
settled is not, and a stale value sampled repeatedly is indistinguishable from
an empty socket. That is what made one cartridge read as 24 KB on one run and
16 KB on the next with no reboot between them. `CART_EMPTY_RETRIES` sets how
many passes, each with a pause and different sample addresses.

A bank reading as one repeated value is not enough on its own to call it empty.
An empty socket on this hardware settles at 0xC0, never 0xFF or 0x00, because
nothing is driving those. A chip whose upper region is unused reads as solid
0xFF, so treating uniform-as-empty drops the end of a cartridge: a 20 KB cart
whose third chip is mostly blank comes back as 16 KB.

Two kinds of tail are trimmed, because a 20 KB cartridge can be built two
ways. `CART_TRIM_MIRROR 0` and `CART_TRIM_BLANK 0` keep whatever the hardware
returns.

An **erased** tail is an 8 KB chip in the last socket with only its lower half
programmed: the rest reads as solid 0xFF, or 0x00 on a mask ROM. Only those two
values count as erased -- any other repeated byte is data.

A **mirrored** tail is trimmed first. Chip selects are 8 KB wide, so a read is
always a multiple of 8 KB, but a cartridge can end with a smaller chip: a 4 KB
chip has A12 unconnected and answers to both halves of its window, so a 20 KB
cartridge reads back as 24 KB with the last 4 KB a copy of the 4 KB before it.
The final bank is halved while its two halves match, down to a floor of 1 KB,
which also covers 2 KB chips. Only the last bank is considered -- a mirrored
bank in the middle would mean something stranger than a small chip, and
trimming there would corrupt everything after it. `CART_TRIM_MIRROR 0` keeps
whatever the hardware returns. It reads the
cartridge exactly as loading does, so a bank-switched one is copied whole
rather than only its first 32 KB. The result is shown with the name used, and
any button returns to the menu.

The decision about USB drive mode has to be made before `tud_init()`, since
after that the port is already enumerating as something. The cartridge cannot
be probed that early and there is no display to ask on, so the device stack
comes up ready for MSC whenever Button 1 is held, and is switched off again if
the menu turns out to say otherwise.

### MegaCart cartridges in the shield

Bank-switched cartridges can be read through the shield as well as from SD.
The banking is on the cartridge, so the reader drives it: selecting bank n is a
read of 0xFFC0+n, which the shield reaches as chip select 3 at offset 0x1FC0+n.

The size is discovered rather than assumed. Selecting a bank beyond the real
count wraps, because the cartridge masks the select with its own bank count, so
the first count at which a bank reads back identical to bank 0 is the real
count. If bank 1 already matches bank 0 there is no banking hardware and it is
read as an ordinary cartridge.

One limitation worth knowing: the select addresses live inside the switchable
window, so the last 64 bytes of a bank cannot be read without changing bank.
They are dumped as 0xFF for every bank except the last, which is readable in
full through the fixed window at 0x8000. No game can keep code there -- it
could not execute it without switching away mid-instruction.

A megabyte takes around 17 seconds to read, with progress shown on screen.

## Activision PCB and saved scores

`ENABLE_ACTIVISION` plays the cartridges that carry a serial EEPROM -- Boxxle,
Black Onyx, Arcomage, Jewel Panic -- and keeps what they save.

```
0x8000-0xBFFF   bank 0, fixed
0xC000-0xFFFF   switchable bank
write 0xFF90 / 0xFFA0 / 0xFFB0   select bank 1 / 2 / 3
write 0xFFC0 / 0xFFD0            SCL low / high
write 0xFFE0 / 0xFFF0            SDA low / high
read  0xFF80                     the SDA line
```

Mapping documented by kevtris on
[AtariAge](https://forums.atariage.com/topic/327053-activision-pcb-bank-switching-and-eeprom/).

This is a different mapper from MegaCart and 64 KB ROMs come in both kinds, so
they are told apart by where the header sits: bank 0 is fixed at 0x8000 on an
Activision PCB, putting its header at offset 0, while a MegaCart keeps its
header at the start of its last bank.

The chip's contents live in PSRAM, past the cartridge buffer, not in SRAM. A
24C256 is 32 KB and a bit-banged bus touches it a few bytes at a time, so
spending 32 KB of the board's scarcest memory on it would be the wrong trade --
it was briefly declared as a plain array and that alone overflowed SRAM.

The EEPROM is bit-banged I2C, so the chip is modelled at the level of the two
bus lines rather than bytes. Both parts are supported and they differ in more
than size: a 24C08 takes a single address byte and carries the top address bits
in the device-address byte, a 24C256 takes two.

Contents are kept in a file beside the ROM, same name with `.sav`. The file's
size records which chip the game has, so `EEPROM_DEFAULT_BYTES` only matters
the first time a game is played. It is written on the way back to the browser,
which is the only way out of a game, and only when the game actually changed
something.

**SD-loaded games only.** A cartridge in the shield has its own real EEPROM on
board; the chip is still emulated so the game runs, but no save file is
written, because it would be a record of the wrong chip's contents.

## Super Game Module

`ENABLE_SGM` adds the Opcode Games expansion: 32 KB of RAM and an AY-3-8910
sound chip, on top of the standard machine rather than instead of it.

| | |
|---|---|
| port 0x53 bit 0 | switches 24 KB in at 0x2000-0x7FFF |
| port 0x7F bit 1 | **clear** maps 8 KB over the BIOS at 0x0000; set restores the BIOS |
| ports 0x50/0x51 | AY register select and write |
| port 0x52 | AY register read |

Both windows start switched out and the AY stays silent until a game writes to
it, so a standard cartridge sees exactly the machine it always did -- verified
by comparing the whole 64 KB address space against the pre-SGM map. With the
upper window in, 0x6000-0x7FFF becomes contiguous RAM rather than the base
machine's 1 KB mirrored eight times. A reset switches both windows out but
leaves the RAM contents alone, as the hardware does.

The AY masks each register to its real width -- write 0xFF to register 1 and
it reads back 0x0F -- and its two unconnected I/O ports read 0xFF. That is not
pedantry: a game detects the module by writing to an AY register and reading it
back, and a chip that returns whatever was written is indistinguishable from no
chip at all.

Note the inverted sense of port 0x7F against port 0x53: bit 1 **clear** enables
the RAM. Reading it the other way round boots to a dead machine.

The module's 32 KB of RAM used to rule out the title chime. Moving cartridges
out of SRAM freed 32 KB, which covers both, so they are now on together by
default.

## Diagnostics

Every bring-up problem in this project was found by measurement rather than
reasoning, and the instruments are still in `include/config.h`, all off by
default. Turn one on when something misbehaves:

| Switch | Shows |
|---|---|
| `BOOT_DIAGNOSTICS` | numbered boot-progress blinks and per-call stage blinks inside `video_init()` and `usb_host_init()`. Costs ~25 s of boot. |
| `EMU_DEBUG_OVERLAY` | emulated Z80 and VDP state in the border below the picture: PC, SP, HL and the byte it points at, interrupt counters, the bytes at PC. |
| `SHOW_CART_DEBUG` | cartridge probe bytes: bank 0 at four addresses, then each chip select. |
| `SHOW_HID_DEBUG` | raw HID reports and the decoded button mask. |
| `SERIAL_INPUT_LOG` | a serial console line for every keypad key and action button change during play, after all mapping. |
| `AUDIO_TEST_TONE` | 440 Hz tone in place of emulator sound, to separate the audio path from the PSG. |
| `AUDIO_CPU_FEED` | bypasses the audio DMA entirely. |

Fatal error codes and the hard-fault blink are always active. A hard fault is
three slow blinks, then a pause, repeating.

## Status

Built and confirmed working on real hardware: 640x480p60 DVI over HSTX, SD card
with the ROM browser, USB drag-and-drop, USB gamepads through the onboard hub,
and audio. The emulator core (Z80, TMS9918A, SN76489A) was verified separately
under AddressSanitizer before any of that.

The cartridge reader shield is the one part still untested -- see the warning
below before fitting it.

`include/config.h` carries a build ID that is shown on the menu's title bar, so
you can always tell whether the board is running the code you just changed. It
also holds switches to compile out video, audio, the USB host and the USB drive
individually, which is how most of the bring-up problems were isolated.

> Formerly `fruitjam-coleco`. The CMake target is `colecojam` and the build
> produces `build/Adafruit_ColecoJam.uf2`. If you are updating an existing
> checkout, delete `build/` after renaming -- the cached target name changes.

## Read this first: the .uf2

**This repository does not ship a prebuilt `.uf2`, and I could not build one for
you.** Two hard reasons:

1. Producing a UF2 requires compiling against the Raspberry Pi Pico SDK plus an
   `arm-none-eabi` toolchain. I had no network access and no toolchain in the
   environment where this code was written, so nothing here has been compiled
   for ARM or run on real hardware.
2. Even if it had been, a UF2 without a legally obtained `COLECO.BIN` on the SD
   card will not boot anything.

What I *did* verify: the emulator core (Z80, TMS9918A, SN76489A, bus) compiles
clean and passes a targeted instruction/flag test suite plus 400 frames of
randomised execution under AddressSanitizer and UndefinedBehaviorSanitizer.
That is the part where correctness is hardest and least forgiving. The hardware
layer — HSTX timing, PIO I2S, the codec register sequence, the shift-register
cartridge reader — is written from datasheets and published pinouts and **has
not been run on a board.** Expect to iterate on it. See
[Known unknowns](#known-unknowns) below for the specific spots.

Building the UF2 takes about two minutes:

```bash
git clone -b 2.3.0 https://github.com/raspberrypi/pico-sdk
cd pico-sdk && git submodule update --init && export PICO_SDK_PATH=$PWD && cd ..

cd Adafruit_ColecoJam
./tools/fetch_deps.sh      # FatFs, Pico-PIO-USB, pico_hdmi, tusb_xinput
./tools/build.sh           # -> build/Adafruit_ColecoJam.uf2
```

Use Pico SDK 2.3.0, the version `CMakeLists.txt` also selects for the VS Code
extension. The XInput driver fetched into `third_party/tusb_xinput` is pinned to
match that SDK's TinyUSB; an SDK carrying a newer TinyUSB fails to compile it
(see the note in `tools/fetch_deps.sh`).

Then hold **Button 1** while pressing **Reset** (or tapping the reset button) to
mount `RP2350` as a USB drive, and drop `Adafruit_ColecoJam.uf2` onto it.

In VS Code: install the recommended extensions, then **Ctrl+Shift+B** runs the
`Build UF2` task. `Flash via picotool` builds and flashes in one step.

---

## Running under the pico-bootLoader

[pico-bootLoader](https://github.com/fhoedemakers/pico-bootLoader) stays resident
in the first 512 KB of flash and flashes and launches whichever application the
user picks from an SD-card menu. A separate build is needed, because the image
has to be linked into the application partition at `0x10080000` instead of the
usual `0x10000000`:

```bash
./fruitjam-build-forbootloader.sh   # -> build_bl_fruitjam/colecojam.uf2
```

**Do not** drag that file onto the Fruit Jam over USB — it is linked for the
application partition and will not boot on its own. Instead put it on the
bootloader's SD card and let the picker flash it:

1. Copy it to `/emu/8/colecojam.uf2` (`8` is the Fruit Jam HW_CONFIG; the
   filename is cosmetic, the loader matches on the name inside the image).
2. Add one row to `/emu/emulators.txt`, the loader's allow-list:
   ```
   colecojam;col;ColecoVision
   ```
   Artwork (`col.png`) already ships with the bootloader.

Holding **Button 1** in the cartridge menu then returns to the picker rather
than to the UF2 bootloader. The standalone build above is unaffected and still
behaves as documented.

The build tree is kept separate from `build/` so the two variants never share
stale objects. See `cmake/BootPartition.cmake` for the flash map and for why
this project cannot use the loader's usual relink helper unmodified.

---

## SD card layout

Format the card **FAT32** and create:

```
/coleco/
    COLECO.BIN          <- the 8 KB ColecoVision BIOS (you must supply this)
    Donkey Kong.ROM
    Cosmic Avenger.ROM
    ...
```

Only files ending in `.ROM` (case-insensitive) appear in the browser. The BIOS
is a copyrighted Coleco ROM image; it is not included here and you need to dump
it from your own console or otherwise obtain it lawfully. The same goes for game
ROMs.

You can also plug the Fruit Jam's **USB-C port** into a computer — the SD card
appears as a removable drive named `Coleco ROM Storage`, so you can drag ROMs
straight onto it. Eject the drive on the computer side and the ROM list
refreshes automatically. While the drive is mounted the menu shows a "USB DRIVE
MODE" screen and stops browsing, so the two sides never write the FAT at once.

---

## Controls

Controllers plug into the two **USB-A** ports. Player 1 is the first pad
enumerated, player 2 the second.

### Supported controllers

Report decoding is ported from the
[pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) family, so
the same devices work here:

- NES- and SNES-style pads with USB ID `081f:e401` (handled as a "MantaPad"),
  such as the
  [Adafruit SNES-layout controller](https://learn.adafruit.com/usb-game-controller-with-snes-like-layout)
- Other USB HID gamepads, including the cheap clones that use the common
  DirectInput report
- DualShock 4, DualSense, PlayStation Classic
- Sega Mega Drive / Genesis Mini, Retro-bit MD Arcade
- XInput pads: Xbox 360 (wired and wireless), Xbox One, Series, original Xbox
- USB keyboards

The ROM browser names the pad it recognised (`DS4`, `X360`, `MSNES`, …), or `??`
if it fell back to generic decoding.

The NES- and SNES-style controllers share one USB identity, and
ColecoJam treats both as the SNES-style controller. On the NES-style
controller, **B** therefore acts as **X**: keypad `3`, or the left side action
button while a keyboard is attached.

NES and SNES controllers on the GPIO header, and the Wii Classic Controller over
I2C, are not supported yet.

### Gamepad

| Gamepad | ColecoVision | With a keyboard attached |
|---|---|---|
| D-pad | Joystick | Joystick |
| Left shoulder | Left side action button | Left side action button |
| Right shoulder | Right side action button | Right side action button |
| Select | Keypad `*` | Keypad `*` |
| Start | Keypad `#` | Keypad `#` |
| A / B | Keypad `1` / `2` | **Right / left side action button** |
| X | Keypad `3` | **Left side action button** |
| Y | Keypad `4` | — |
| Select + A / B / X / Y | Keypad `5` / `6` / `7` / `8` | — |
| Start + A / B | Keypad `9` / `0` | — |

Combinations take priority: holding Select and pressing A sends `5`, not `*`
then `1`. Select or Start on its own is what produces `*` or `#`.

Attaching a keyboard makes the whole keypad directly reachable, so the pad stops
standing in for it and becomes a plain ColecoVision controller — a joystick and
two action buttons, which is all the original hardware had.

On controllers labelled like an Xbox or PlayStation pad, the buttons follow the
Nintendo position rather than the printed label: the right face button acts as
**A** and the bottom one as **B**, as on the Adafruit controllers. With a
keyboard attached, the right face button is therefore the right side action
button and the bottom one the left.

### Keyboard

A keyboard never occupies a controller port. It merges into player 1, so a
gamepad there keeps the joystick while the keyboard supplies the keypad.

| Keyboard | ColecoVision |
|---|---|
| Arrow keys | Joystick |
| `Z` / `X` | Left / right side action button |
| `0`–`9` (number row or numeric keypad) | Keypad `0`–`9` |
| `Shift`+`8`, numeric keypad `*` | Keypad `*` |
| `Shift`+`3`, numeric keypad `/` | Keypad `#` |
| `A` / `S` | Keypad `*` / `#` |

Many cartridges read the keypad on their title screen to pick a game mode or a
skill level, which is what the digits are for.

### Menu

D-pad or arrow keys browse (left/right page). **A** or **Start** on a pad loads
the selected game; on a keyboard, `X`, `S` or **Enter** does. Board **Button 2**
and **Button 3** also work if nothing is connected yet. The list reopens on the
game you were last playing.

### Title sequence

On the way into the cartridge menu the logo fades up in the centre of the
screen, a light sweeps across it, it holds, then fades out and the menu fades
in. Any controller or board button abandons it, and that press is then ignored
until it is released, so skipping the titles cannot also pick a game off the
menu underneath.

```c
#define TITLE_SEQUENCE        1     // 0 disables it entirely
#define TITLE_SYNC_DELAY_MS  2000   // held black while the display locks on
#define TITLE_FADE_IN_MS      700
#define TITLE_SWEEP_DELAY_MS  500   // pause after the fade before the sweep
#define TITLE_SOUND_DELAY_MS  500   // pause after the fade before the chime
#define TITLE_SWEEP_MS       1000
#define TITLE_HOLD_MS        3000
#define TITLE_FADE_OUT_MS     700
#define TITLE_MENU_FADE_MS    500   // cartridge menu fades in
```

The sync delay matters more than it looks. HDMI sinks commonly take two to
five seconds to lock onto a signal after the source starts driving it, and the
title begins about a second into boot -- so without the delay the fade and the
sweep can both finish while the screen is still blank, and the first thing
actually shown is the logo already sitting still. Lower it if your display
locks quickly.

A start-up chime plays `TITLE_SOUND_DELAY_MS` after the fade-in completes.
That is a separate setting from the sweep's delay so the two can be moved
apart, but they default to the same value and so begin together. Set
`TITLE_SOUND` to 0 to drop it and reclaim about 17 KB of SRAM. Nothing waits
for it: the sweep and hold carry on over it, and if the titles are abandoned it
keeps sounding under the menu rather than being cut off.

The sample lives in `src/ui/sound_start.h` at 4900 Hz, 16-bit mono, 1.8 s,
about 17 KB. Every one of those numbers is set by the RAM budget -- see below.
4900 Hz because 99.9% of the sound's energy is below 2 kHz, so a 2450 Hz
Nyquist loses nothing audible, and 44100 / 4900 is exactly 9. Truncated at
1.8 s because what follows is reverb decay already below -38 dB, with a long
fade over the cut. 16-bit rather than 8-bit because 8-bit measured only about
33 dB signal-to-noise even dithered, audible as granularity on a decay.
Playback interpolates rather than repeating samples, avoiding the hard edge
sample-and-hold puts on a quiet chime.

## RAM is the binding constraint

The image is built `copy_to_ram`, so everything -- code, constants and
variables alike -- has to fit in the 512 KB main SRAM region. It is close to
full:

| | |
|---|---|
| framebuffer 320x240 RGB565 | 150 KB |
| cartridge buffer | 32 KB |
| VDP VRAM | 16 KB |
| BIOS | 8 KB |
| everything else, code included | the rest |

There is roughly 20 KB spare. A new feature that needs more than that has to
take it from somewhere: `TITLE_SOUND 0` frees about 17 KB, and turning off
`VIDEO_DRIVER_PICO_HDMI` removes the `copy_to_ram` requirement altogether at
the cost of HDMI audio. If a link fails with ``region `RAM' overflowed``, this
is why.

It does not play when a cartridge is fitted -- that boots straight into the
game -- nor when the SD card is mounted as a USB drive, since that screen owns
the display.

Fading the menu *in* cannot be done by scaling the framebuffer, because the
original is gone the moment it is scaled down and there is no room for a second
150 KB copy. Instead the drawing primitives take a global brightness and the
menu is re-rendered at each step. The light sweep pushes colours toward white
rather than adding a constant, so it brightens the dark outline as much as the
lit letters and cannot wrap round a channel that is already at maximum. It
lights only the logo's ink: the background and the insides of the letters are
pure black in the source, so testing for that isolates the artwork exactly and
the highlight never shows as a rectangle sliding over the black. Its falloff is
squared rather than linear, so the band fades out at its edges instead of
ending on a hard line.

### Screen saver

After a period with no input the menu gives way to the logo bouncing around the
screen. Any pad direction, face button or board button restores the list, and that
input is then ignored until it is released, so it cannot also move the cursor
or start a game. Two settings in `include/config.h`:

```c
#define MENU_SAVER_TIMEOUT_S  120   // idle seconds before it starts; 0 disables
#define MENU_SAVER_FADE_MS    500   // menu fades away around the logo
#define MENU_SAVER_PAUSE_MS   500   // then everything holds still
#define MENU_SAVER_BOUNCE_S   5     // seconds from the top of the screen to the bottom
```

It runs in three stages: the menu fades to black around the logo, which stays
put; everything holds still for the pause; then the logo sets off from exactly
where it was. Either time may be 0 to skip that stage.

The fade scales the framebuffer in place, since there is no room for a second
150 KB copy to fade from. To land on `original * (1 - i/N)` at step `i`, given
the buffer already holds `original * (1 - (i-1)/N)`, each step multiplies by
`(N-i)/(N-i+1)` -- exact, and reaching zero on the final step. Rows are split
around the logo so it is never touched.

The horizontal speed matches the vertical one in pixels per second, so the logo
travels at 45 degrees and the two axes drift in and out of phase rather than
retracing a single path. Position comes from the clock rather than a per-frame
step, so the speed does not depend on how busy the menu loop is.

The bounce begins at the logo's position on the menu, so the changeover looks
like the list and header falling away from around it rather than the logo
jumping to a corner. Each frame draws the logo at its new position first and
then paints out only the sliver it has left behind, so the framebuffer never
holds a frame without it. Redraws are paced to the vertical blank, and the USB stacks keep being
serviced while waiting for it.

### If a controller stops responding

Two safeguards run from the USB host task, both added after this was seen in
practice:

- A HID report request that fails is retried. Upstream printed an error and
  gave up, and since that request is the only thing keeping reports flowing, a
  single transient failure silenced the pad until it was physically replugged.
- A HID interrupt transfer can also be lost *without* anything reporting a
  failure, which leaves the pad silent while it still shows as connected and
  correctly assigned (`P1 ...` with no phantom second slot). Once a second the
  host task calls `tuh_hid_receive_report()` on every mounted interface: it
  returns false and does nothing when a transfer is already pending, and true
  only when none was outstanding -- which for a mounted interface means the
  last one vanished, and the call has just re-armed it. A plain timeout
  watchdog would not work here, because an untouched controller legitimately
  sends nothing for minutes.
- Player slots are keyed on the USB device address, so a controller that drops
  off and returns at a new address would leave its old slot allocated forever.
  The menu showed this as `P1 ... P2 ...` with one controller plugged in, and
  the pad unresponsive because the game was reading the dead slot. Slots whose
  device is no longer on the bus are now released, without relying on the
  unmount callback, which is not delivered when a device vanishes through an
  error rather than a clean disconnect.

### Button 1

| Where | Action | Result |
|---|---|---|
| During a game | **press** | back to the cartridge menu, immediately |
| In the menu | **hold 1 s** | leave ColecoJam: back to the picker when launched from the pico-bootLoader, otherwise into the UF2 bootloader |
| On an error screen | **hold** | same as holding it in the menu |
| At power-on | press within 2 s | mount the SD card as a USB drive |

Returning to the menu keeps the SD card mounted and the BIOS loaded, so it is
instant rather than a reboot, and the sound chip is silenced on the way out.
Pressing Button 1 during a game from the cartridge shield also lands in the SD
menu.

So leaving ColecoJam from a game is two steps: tap Button 1 to reach the menu,
then hold it. Earlier builds went straight to the picker on a one-second hold
during play; that hold now belongs to the menu, because a press in a game is
the quick way back to the list.

---

## Cartridge reader shield

If the [Adafruit ColecoJam Cartridge
Reader](https://github.com/cogliano/Fruit_Jam_ColecoVision_Cartridge_Reader)
shield is fitted with a cartridge seated, the emulator detects it at boot, dumps
it to RAM and runs it — the SD browser is skipped entirely. Pull the cartridge
to get the menu back.

Detection is belt-and-braces: a card-detect pin plus a check that the first two
bytes are `AA 55` or `55 AA`, which every ColecoVision cartridge begins with.
Cartridge size is found by probing each 8 KB bank for a floating (all-`0xFF`)
response, so 8/16/24/32 KB carts all read correctly.

**The shield pin assignments in `src/hw/cart_reader.cpp` are defaults I could
not verify** — see [Known unknowns](#known-unknowns). Check them before you plug
a cartridge in.

---

## How it works

```
core 0                                    core 1
------                                    ------
Z80  (3.579545 MHz, 228 T-states/line)    psg_render() -> I2S DMA
TMS9918A (renders 1 scanline at a time)
SN76489A register writes
USB host + device polling
                |
                v
    320x240 RGB565 framebuffer
                |
        HSTX DMA command list
                |
         640x480p60 DVI out
```

**CPU.** Full Z80A interpreter: documented set plus `SLL`, `IN F,(C)`,
`IXh`/`IXl` halves, the DDCB register-copy side effect, and the undocumented
Y/X flag behaviour that `CP`, `LDIR` and the block compares depend on. The VDP
interrupt is wired to `/NMI`, which is how the real ColecoVision does it.

**Video.** 256×192 is centred in a 320×240 framebuffer (32 px left/right, 24 px
top/bottom border) and scaled 2× to 640×480 by the HSTX peripheral itself — no
CPU cost, no PIO, and a true 60 Hz that every monitor accepts. All four
TMS9918A modes are implemented (Graphics I, Graphics II, Multicolour, Text)
along with sprites, the 4-sprites-per-line limit, the fifth-sprite flag and
collision detection. Rendering is per-scanline so mid-frame register changes
work, which several titles use for status bars.

**Audio.** Three tone channels and one noise channel at 44.1 kHz. Counters run
at the real chip's clock/16 and are averaged down to the sample rate, so
high-frequency channels don't alias into whine.

**Timing.** The VDP frame is 59.92 Hz against a 60 Hz display, so roughly one
frame is duplicated every twelve seconds. That's imperceptible and far better
than tearing.

---

## Known unknowns

Places where I had to make a judgement call, roughly in order of how likely they
are to need adjusting:

1. ~~**Cartridge reader pin map**~~ — resolved from the reference project,
   [cogliano/Fruit_Jam_ColecoVision_Cartridge_Reader](https://github.com/cogliano/Fruit_Jam_ColecoVision_Cartridge_Reader),
   which needs the ColecoVision Game Cartridge Adapter Shield from DanTheGeek.

   The address bus and CS 0xE000 are clocked out over **SPI0** — the same bus
   as the microSD card — into two chained 74HC595s, latched by GPIO 43. A
   16-bit word sent MSB first carries CS 0xE000 in bit 15 and A14..A0 in bits
   14..0. Three chip selects sit on GPIO 10, 20 and 21; the data bus is on
   GPIO 7, 45, 41, 42, 44, 6, 8 and 9 (D0..D7), read-only with pull-downs.

   Two things follow from the wiring and are handled in `cart_reader.cpp`:

   - Reading bank 3 takes **two latches**, address first with CS 0xE000 still
     high, because that chip select shares the shift register with the address
     and a single latch would make both valid at the same instant.
   - The shield borrows the **audio codec's I2C bus** (GPIO 20/21) and the
     **debug UART** (GPIO 8/9). `cart_present()` gives them back when no
     cartridge is found, and the codec is configured before any of this runs.

   There is no cartridge-detect line, so presence is decided by the `AA 55` /
   `55 AA` header signature.

   The cartridge is read **once**, at boot, into a 32 KB RAM buffer, and the
   shield's pins are released immediately afterwards. The emulator's bus reads
   come from that buffer, never from the hardware — so the shield adds no
   per-instruction cost, and a cartridge can be removed mid-game without
   affecting anything. Changing cartridges needs a reset.

2. **HSTX register setup** (`src/hw/video_hstx.cpp`). The TMDS encoder and
   serialiser configuration follows the pico-examples `dvi_out_hstx_encoder`
   demo. Cross-check `expand_tmds` / `expand_shift` / `csr` against the exact
   SDK version you build with; these fields moved around between early RP2350
   SDK releases.

3. **SD card detect pin.** The Adafruit pinout page lists `SD_CARD_DETECT` as
   GPIO34, which collides with `SD_SCK` — clearly a typo in the guide. Card
   detect isn't needed, so it's disabled (`PIN_SD_CARD_DETECT -1`).

4. **TLV320DAC3100 PLL constants.** The J/D/NDAC/MDAC values in `audio.cpp`
   target 44.1 kHz from a 32×fs BCLK with BCLK as the PLL input. If audio comes
   out at the wrong pitch, that's the block to revisit.

5. **Gamepad report decoding.** `hid_app.cpp` recognises a list of controllers
   by USB vendor and product ID — the Adafruit pads among them — and parses the
   report descriptor for the rest, falling back to the common DirectInput-style
   8-byte layout. That covers most cheap clones, but an unusual pad may still
   need its own case. Set `SHOW_HID_DEBUG` in `include/config.h` to put the live raw
   report in the menu footer, or read the serial console (below) to see what a
   device enumerates as.

6. **Debug console.** `printf` output goes to UART1 at 115200 baud on **GPIO 8**
   (TX) and **GPIO 9** (RX), the board default. The onboard ESP32-C6 shares those
   pins, so the output may not be readable. `CMakeLists.txt` describes the
   alternative, UART0 on GPIO 44/45 (the A4/A5 header pins), which works only
   with the cartridge reader shield removed: those pins are also cartridge data
   lines. The firmware prints its build and date at boot.

7. **Megacart / bank-switched ROMs are not supported.** The bus implements the
   standard 32 KB cartridge window only. Anything larger than 32 KB is
   truncated.

---

## Licence

The emulator is derived in structure from
[Gearcoleco](https://github.com/drhelius/Gearcoleco) by Ignacio Sánchez, which
is **GPL-3.0**. This project is therefore GPL-3.0 as well — see `LICENSE`. If
you distribute binaries, distribute the source too.

Third-party components fetched by `tools/fetch_deps.sh`:

- [FatFs](http://elm-chan.org/fsw/ff/) — ChaN, BSD-style licence
- [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) — MIT
- [TinyUSB](https://github.com/hathach/tinyusb) — MIT (ships with the Pico SDK)
- [pico_hdmi](https://github.com/fliperama86/pico_hdmi) — public domain (Unlicense)
- [tusb_xinput](https://github.com/PicoPlus-devel/tusb_xinput) — MIT, Ryan Wendland

The USB controller and keyboard decoding in `src/hw/hid_app.cpp` and
`src/hw/gamepad.*` is ported from `pico_shared` in
[pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) (GPL-3.0),
originally by Shuichi Takano.

The HSTX video approach and the menu design follow
[fhoedemakers' pico-snesPlus / pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus).

No ROMs, BIOS images or other Coleco copyrighted material are included.
