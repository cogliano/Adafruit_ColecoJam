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
