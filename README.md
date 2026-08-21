# Adafruit_ColecoJam

A ColecoVision emulator for the **Adafruit Fruit Jam** (RP2350B), written in
C++ against the Pico SDK and formatted for Visual Studio Code.

Output goes to the Fruit Jam's HDMI/DVI port over HSTX, sound goes to the
onboard TLV320DAC3100, games load from the microSD card (or straight off a real
cartridge, if you have the reader shield), and control is via one or two
SNES-layout USB gamepads.

---

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
git clone -b 2.1.1 https://github.com/raspberrypi/pico-sdk
cd pico-sdk && git submodule update --init && export PICO_SDK_PATH=$PWD && cd ..

cd Adafruit_ColecoJam
./tools/fetch_deps.sh      # FatFs + Pico-PIO-USB
./tools/build.sh           # -> build/Adafruit_ColecoJam.uf2
```

Then hold **Button 1** while pressing **Reset** (or tapping the reset button) to
mount `RP2350` as a USB drive, and drop `Adafruit_ColecoJam.uf2` onto it.

In VS Code: install the recommended extensions, then **Ctrl+Shift+B** runs the
`Build UF2` task. `Flash via picotool` builds and flashes in one step.

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

USB gamepads plug into the two **USB-A** ports. Player 1 is the first pad
enumerated, player 2 the second. Mapping follows the
[Adafruit SNES-layout controller](https://learn.adafruit.com/usb-game-controller-with-snes-like-layout):

| Gamepad | ColecoVision |
|---|---|
| D-pad | Joystick |
| Left shoulder | Left side action button |
| Right shoulder | Right side action button |
| Select | Keypad `*` |
| Start | Keypad `#` |
| A / B / X / Y | Keypad `1` / `2` / `3` / `4` |
| Select + A / B / X / Y | Keypad `5` / `6` / `7` / `8` |
| Start + A / B | Keypad `9` / `0` |

Combinations take priority: holding Select and pressing A sends `5`, not `*`
then `1`. Select or Start on its own is what produces `*` or `#`.

In the menu, D-pad browses (left/right page), **A** or **Start** loads. Board
**Button 2** and **Button 3** also work if no pad is connected yet. Holding
**Button 1** at any time reboots into the UF2 bootloader.

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

1. **Cartridge reader pin map** (`src/hw/cart_reader.cpp`). GitHub blocked
   automated access to the reader repo, so I could not read its pin table. The
   assignments there follow the standard 74HC595 shift-register topology that
   the reader descends from (15 address lines through two chained registers,
   8 data lines and 4 chip selects on direct GPIO) mapped onto the Fruit Jam's
   broken-out header in the obvious order. **Confirm these against the
   CircuitPython source before connecting a cartridge** — driving a data pin as
   an output into the cartridge's output could damage either board. Every pin is
   a single `#define`.

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

5. **Gamepad report decoding.** `usb_host.cpp` decodes the common
   DirectInput-style 8-byte HID report rather than parsing the report
   descriptor. That covers the Adafruit controller and most cheap clones, but an
   unusual pad may need its own case.

6. **Megacart / bank-switched ROMs are not supported.** The bus implements the
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

The HSTX video approach and the menu design follow
[fhoedemakers' pico-snesPlus / pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus).

No ROMs, BIOS images or other Coleco copyrighted material are included.
