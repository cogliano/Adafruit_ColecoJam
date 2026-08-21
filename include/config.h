// config.h -- Adafruit Fruit Jam (RP2350B) board configuration
//
// Pin assignments below come from the official Fruit Jam pinout page:
// https://learn.adafruit.com/adafruit-fruit-jam/pinout
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ---------------------------------------------------------------------------
// USB CLOCK TEST MODE -- diagnostic only, video will not work.
//
// Set to 1 to run the most reference-like configuration possible: clk_sys at
// 120 MHz (the value the known-working PIO-USB examples use), pll_usb left
// alone at its stock 48 MHz, and no HSTX clock set up at all. Video is forced
// off because there is no 126 MHz source in this mode.
//
// This answers one question: does PIO-USB work on this board with this code
// once our unusual three-PLL clock tree is out of the picture? If the host
// still hangs here, the clock tree was never the problem.
//
// Defined first because the clock settings below depend on it.
// ---------------------------------------------------------------------------
#define USB_CLOCK_TEST_MODE 0

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------
// 252 MHz gives an exact 126 MHz HSTX clock (252/2), which is 5x the 25.2 MHz
// pixel clock of 640x480p60 -- so DVI comes out at a true 60 Hz that every
// monitor accepts. It also leaves ~70x realtime headroom over the 3.58 MHz Z80.
// Clocks.
//
// Three hard constraints, which cannot all be met from one PLL:
//
//   Pico-PIO-USB   clk_sys must be 120 or 240 MHz. Anything else and the host
//                  stack hangs during tuh_task().
//   DVI 640x480p60 clk_hstx must be 126 MHz, i.e. 5 x the 25.2 MHz pixel clock.
//   Native USB     clk_usb must be 48 MHz for the MSC device.
//
// The RP2350 has two PLLs, so:
//
//   pll_sys = 240 MHz -> clk_sys  = 240 MHz         (PIO-USB satisfied)
//                     -> clk_usb  = 240 / 5 = 48    (native USB satisfied)
//   pll_usb = 126 MHz -> clk_hstx = 126 MHz         (DVI exact, 60.00 Hz)
//
// pll_usb is retuned away from its default 48 MHz and no longer feeds clk_usb
// at all -- see setup_clocks() in main.cpp, which must move clk_usb onto
// pll_sys BEFORE retuning pll_usb.
//
// 240 MHz still leaves ~67x realtime headroom over the 3.58 MHz Z80.
#if USB_CLOCK_TEST_MODE
  #define CV_SYS_CLK_KHZ    120000    // matches the working PIO-USB examples
#else
  #define CV_SYS_CLK_KHZ    240000
#endif
#define HSTX_CLK_HZ         126000000

// pll_usb retune target for clk_hstx: VCO 1512 MHz / (6 x 2) = 126 MHz.
//
// Deliberately prefixed CV_. PLL_USB_POSTDIV1/2 and PLL_USB_VCO_FREQ_HZ are
// SDK-owned macros that hardware/clocks.h uses to configure pll_usb to 48 MHz
// at startup. Reusing those names silently changed the SDK's own idea of the
// USB PLL in every file that included config.h before clocks.h -- which was
// audio.cpp and video_hstx.cpp. Same trap as the SYS_CLK_KHZ collision.
#define CV_PLL_USB_VCO_HZ   1512000000
#define CV_PLL_USB_POSTDIV1 6
#define CV_PLL_USB_POSTDIV2 2

// ---------------------------------------------------------------------------
// DVI / HSTX  (GPIO12-19, fixed by the HSTX peripheral)
// ---------------------------------------------------------------------------
#define PIN_HSTX_CK_N       12
#define PIN_HSTX_CK_P       13
#define PIN_HSTX_D0_N       14
#define PIN_HSTX_D0_P       15
#define PIN_HSTX_D1_N       16
#define PIN_HSTX_D1_P       17
#define PIN_HSTX_D2_N       18
#define PIN_HSTX_D2_P       19

// ---------------------------------------------------------------------------
// Display geometry
// ---------------------------------------------------------------------------
#define FB_WIDTH            320
#define FB_HEIGHT           240
#define DVI_WIDTH           640         // pixel-doubled on the wire
#define DVI_HEIGHT          480

// The ColecoVision's 256x192 image centred inside 320x240.
#define CV_ORIGIN_X         ((FB_WIDTH  - 256) / 2)    // 32
#define CV_ORIGIN_Y         ((FB_HEIGHT - 192) / 2)    // 24

// ---------------------------------------------------------------------------
// MicroSD (SPI0)
// ---------------------------------------------------------------------------
#define PIN_SD_SCK          34
#define PIN_SD_MOSI         35
#define PIN_SD_MISO         36
#define PIN_SD_CS           39
#define SD_SPI_PORT         spi0
#define SD_BAUD_INIT        400000      // <=400 kHz during card init
#define SD_BAUD_FAST        20000000

// Card detect, confirmed from the SDK board header
// (ADAFRUIT_FRUIT_JAM_SD_CARD_DETECT_PIN). Not currently used by the driver,
// but correct here for anyone who wants it.
#define PIN_SD_CARD_DETECT  33

// ---------------------------------------------------------------------------
// Audio: TLV320DAC3100 over I2S, configured over I2C0
// ---------------------------------------------------------------------------
#define PIN_I2S_DATA        24
#define PIN_I2S_MCLK        25
#define PIN_I2S_BCLK        26
#define PIN_I2S_WS          27
#define PIN_I2S_IRQ         23
#define PIN_PERIPH_RESET    22          // shared with the ESP32-C6

#define PIN_I2C_SDA         20
#define PIN_I2C_SCL         21
#define I2C_PORT            i2c0
#define I2C_BAUD            100000
#define TLV320_I2C_ADDR     0x18

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_BUF_SAMPLES   512         // per DMA half-buffer

// ---------------------------------------------------------------------------
// USB host (PIO-USB on the CH334F hub's upstream port)
// ---------------------------------------------------------------------------
#define PIN_USB_HOST_DP     1
#define PIN_USB_HOST_DM     2           // must be DP+1 for pico-pio-usb
#define PIN_USB_HOST_5V_EN  11

// Polarity of the USB-A power enable. The board header names the pin but not
// its sense, so this is an assumption. If the status line reports 0 devices
// with a controller plugged in, try flipping this to 0 -- with the wrong
// polarity the ports are simply unpowered and nothing can ever enumerate.
#define USB_HOST_5V_ACTIVE_HIGH 1

// ---------------------------------------------------------------------------
// Buttons / LEDs
// ---------------------------------------------------------------------------
#define PIN_BUTTON1         0           // also BOOTSEL
#define PIN_BUTTON2         4
#define PIN_BUTTON3         5
#define PIN_NEOPIXEL        32
#define PIN_LED             29          // active LOW (anode tied to 3V3)

// ---------------------------------------------------------------------------
// SD card layout
// ---------------------------------------------------------------------------
#define COLECO_DIR          "coleco"
#define COLECO_BIOS_FILE    "coleco/COLECO.BIN"
#define ROM_EXTENSION       ".ROM"
#define MAX_ROM_ENTRIES     512
#define MAX_FILENAME_LEN    64

// ---------------------------------------------------------------------------
// Cartridge reader shield
// ---------------------------------------------------------------------------
// Set to 0 to compile the shield support out entirely.
#define ENABLE_CART_READER  1

// Set to 0 to skip video_init() entirely. Everything still runs -- USB, SD,
// the emulator -- there is just no DVI output and the menu draws into a
// framebuffer nobody scans out. Use this to prove whether a USB or SD problem
// is caused by the HSTX driver: if it goes away with video off, the video
// driver's DMA or interrupt load is the culprit, not USB or SD.
#define ENABLE_VIDEO        1

// Set to 0 to compile out the USB host stack (gamepads). The MSC drag-and-drop
// device on the USB-C port is unaffected, and the menu is still navigable with
// board Buttons 2 and 3. Use this to confirm whether a hang is coming from
// Pico-PIO-USB: the host stack is by far the most timing-sensitive thing in
// this project, and it is sensitive to clk_sys in particular.
#define ENABLE_USB_HOST     1

// Which core runs the USB host stack.
//   1 = core 1 (default; keeps PIO-USB away from the video interrupt)
//   0 = core 0 (matches the simplest reference examples)
// tuh_init() and tuh_task() must run on the SAME core, which this guarantees.
// Provided for bisecting a tuh_init() hang, not as a tuning knob.
#define USB_HOST_ON_CORE1   1

// Set to 0 to compile out audio entirely. In the confirmed-working USB tester,
// core 1 runs NOTHING but tuh_task() -- no audio, no PIO2, no second DMA pair.
// Turning this off makes our core 1 match that exactly, which is worth testing
// before concluding tuh_init() is unfixable here.
#define ENABLE_AUDIO        1

// Set to 1 to show the raw HID report from controller 0 as hex in the menu
// footer, refreshed live, along with the report length and a running count of
// reports received.
//
// This is how the button mapping in usb_host.cpp was worked out, and it is the
// tool to reach for if a different controller decodes wrongly: hold each
// direction and button in turn and read off which byte and bit changes. The
// menu grows a fourth footer row when this is on.
// Require Button 1 to be pressed during a short window at boot before the SD
// card is offered to a host computer as a USB drive.
//
//   1 = drive only appears if Button 1 is pressed during the window below
//   0 = drive always appears (previous behaviour)
//
// Worth having on: while a host has the volume mounted the emulator must not
// touch the filesystem, so the menu blocks. With the gate on, plugging into a
// PC purely for power no longer locks you out of the browser.
#define MSC_REQUIRES_BUTTON 1

// How long to watch for that press, in milliseconds. The window costs this
// much on every boot when nothing is pressed, so keep it short. Ignored when
// MSC_REQUIRES_BUTTON is 0.
#define MSC_BUTTON_WINDOW_MS 2000

// Set to 1 to replace the emulator's sound with a steady 440 Hz tone. This
// separates two very different faults: no tone means the I2S path, the codec
// or the amplifier is broken; a tone but no game sound means the problem is in
// the PSG or in how the emulator drives it.
// Which PIO block runs the I2S output: 0, 1 or 2.
//
// PIO2 by default, because Pico-PIO-USB occupies PIO0 (its TX program) and
// PIO1 (RX and end-of-packet). PIO1 still has two free state machines though,
// so switching to 1 is worth trying if PIO2 misbehaves -- it is the one block
// in this design that nothing else has exercised.
#define AUDIO_PIO_INSTANCE  2

// Feed I2S from the CPU instead of by DMA.
//
// Diagnostic. core 1 pushes each sample with pio_sm_put_blocking() rather than
// handing buffers to a DMA chain. If sound appears in this mode the state
// machine and the codec are both fine and the fault is in the DMA setup; if it
// is still silent -- or the push blocks forever, freezing the pad in the menu
// -- the state machine is not pulling from its FIFO and the DMA was never the
// problem.
// Which output the codec drives.
//   1 = mono speaker connector (class-D amp; needs the board on 5V)
//   0 = headphone jack only
// Both can be on at once, but the class-D amp adds noise to the headphone
// output, so Adafruit recommend against it.
// Page 1, register 0x23: output mixer routing.
//
// Sources disagree on the bit assignment for this part: some place DAC_L->HPL
// and DAC_R->HPR at D6/D2 (0x44), others at D7/D3 (0x88) with D6/D2 being the
// analog inputs instead. 0x44 alone produced running DACs, powered headphone
// drivers and complete silence -- consistent with having routed the analog
// inputs rather than the DAC.
//
// A confirmed-working sequence for this part uses 0x44, so that is the
// default. 0x88 is worth trying only if 0x44 produces nothing.
#define TLV_HP_ROUTING      0x44

// Page 1 registers 0x24 / 0x25: analog volume to HPL / HPR.
//   D7    1 = HPL/HPR connected to the analog volume block, 0 = disconnected
//   D6:D0 attenuation, 0x00 = 0 dB, larger = quieter
//
// 0x80 = connected at 0 dB. A widely circulated script uses 0x70 and comments
// it "routed, 0 dB", but 0x70 has D7 clear and therefore disconnects the path
// entirely -- following that comment cost two builds here.
//
// If it is too loud, keep D7 set and raise the low bits: 0x88 is about -4 dB,
// 0x90 about -8 dB.
#define TLV_HP_VOLUME       0x80

#define AUDIO_USE_SPEAKER   0

#define AUDIO_CPU_FEED      0

#define AUDIO_TEST_TONE     0

#define SHOW_HID_DEBUG      0

// Build identifier, shown on the cartridge menu's title bar.
//
// Exists so "is the board actually running the code I just changed?" is
// answerable at a glance. A stale binary once produced button readings that
// perfectly matched an older decoder, and it took a full round of analysis to
// realise the source and the firmware had diverged. Bump this whenever you
// change something you intend to test.
#define FJC_BUILD_ID        "build 28"


// ---------------------------------------------------------------------------
// USB clock test mode overrides. Placed last so they win over the settings
// above regardless of where those appear in this file.
// ---------------------------------------------------------------------------
#if USB_CLOCK_TEST_MODE
  #undef  ENABLE_VIDEO
  #define ENABLE_VIDEO      0         // no 126 MHz source in this mode
#endif
