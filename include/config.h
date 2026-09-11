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
// ---------------------------------------------------------------------------
// Which video driver to build.
//
//   VIDEO_DRIVER_HSTX      the built-in DVI driver in video_hstx.cpp. Video
//                          only; sound must go to the codec.
//   VIDEO_DRIVER_PICO_HDMI the pico_hdmi library, which adds HDMI data islands
//                          and therefore sound over the same cable.
//
// pico_hdmi owns core 1 and derives clk_hstx as clk_sys / PICO_HDMI_HSTX_CLK_DIV,
// so clk_sys must be 126 MHz times that divider -- see the clock block above.
// ---------------------------------------------------------------------------
#define VIDEO_DRIVER_HSTX       0
#define VIDEO_DRIVER_PICO_HDMI  1

// NOTE: if you change this, change VIDEO_DRIVER_IS_PICO_HDMI in CMakeLists.txt
// to match. CMake cannot read this header, and it needs to know: the library
// driver requires the image to run from RAM rather than XIP.
#define VIDEO_DRIVER            VIDEO_DRIVER_PICO_HDMI

#define USB_CLOCK_TEST_MODE 0

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------
// 252 MHz gives an exact 126 MHz HSTX clock (252/2), which is 5x the 25.2 MHz
// pixel clock of 640x480p60 -- so DVI comes out at a true 60 Hz that every
// monitor accepts. It also leaves ~70x realtime headroom over the 3.58 MHz Z80.
// Clocks.
//
// clk_sys MUST be an integer multiple of 12 MHz: Pico-PIO-USB derives its USB
// bit timing by dividing clk_sys, and a non-integer ratio produces invalid USB
// signalling that takes the shared TinyUSB stack down with it.
//
// The rest depends on which video driver is built:
//
//   VIDEO_DRIVER_PICO_HDMI
//     The library sets clk_hstx = clk_sys / PICO_HDMI_HSTX_CLK_DIV, an integer
//     divide, and 640x480p60 needs 126 MHz. Divider 2 gives clk_sys = 252 MHz,
//     which is 21 x 12 -- so HDMI and PIO-USB agree on one PLL and pll_usb is
//     left alone at its stock 48 MHz for the USB device.
//
//   VIDEO_DRIVER_HSTX
//     Our own driver can source clk_hstx from anywhere, so clk_sys runs at
//     240 MHz for PIO-USB and clk_hstx comes from a retuned pll_usb. That
//     needs clk_usb re-sourced onto pll_sys first -- see setup_clocks().
#if VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
  #define CV_SYS_CLK_KHZ    252000
  // NOTE: the matching divider is NOT set here. pico_hdmi reads it as a CMake
  // cache variable (PICO_HDMI_HSTX_CLK_DIV) and never includes this file, so
  // defining it here has no effect whatsoever. See CMakeLists.txt.
#else
  #define CV_SYS_CLK_KHZ    240000
#endif

#define HSTX_CLK_HZ         126000000

// pll_usb retune target, used only by VIDEO_DRIVER_HSTX:
// VCO 1512 MHz / (6 x 2) = 126 MHz. Prefixed CV_ because PLL_USB_POSTDIV1/2
// and PLL_USB_VCO_FREQ_HZ are SDK-owned names.
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

// --- Cartridge reader shield -----------------------------------------------
// Pin assignments taken from the reference CircuitPython implementation:
// https://github.com/cogliano/Fruit_Jam_ColecoVision_Cartridge_Reader
//
// The shield borrows pins the rest of the board also uses. GPIO 20/21 are the
// audio codec's I2C bus and GPIO 8/9 are the debug UART, so cart_present()
// hands them back when no cartridge is found. Configure the codec BEFORE
// touching the cartridge, which is the order main() already uses.
// LATCH PIN VARIANT. The reference project contradicts itself here, and the
// two sources cannot both be right:
//
//   0 = latch on GPIO 43 (board A3), data bit 6 on GPIO 8   [code.py]
//   1 = latch on GPIO 8,             data bit 6 on GPIO 43  [README table]
//
// code.py drives A3 as the latch and lists D8 among the data pins; the
// README's wiring table runs GPIO 8 to 74HC595 pin 12, which is RCLK. The
// comments in code.py mention "version 1" and "version 2" boards, so these are
// probably different shield revisions.
//
// Symptom of getting it wrong: the address never reaches the cartridge, so
// every offset returns the same byte.
//
// Both variants showed that symptom while the shift registers were being
// clocked on the wrong SPI bus, so neither was actually ruled out. Back to 0
// (code.py's reading) now that SPI1 is used; try 1 if the address is still
// dead.
#define CART_LATCH_VARIANT  0

// Address lines and CS 0xE000 are clocked out over SPI1 on GPIO 30 (SCK) and
// GPIO 31 (MOSI).
//
// NOT the SD card's SPI0. GPIO 34/35 serve the microSD socket directly and are
// not brought out on the 2x16 header, so a shield cannot reach them -- driving
// them talked to nothing, which is why chip selects worked while the address
// bus stayed dead. GPIO 28/30/31 are SPI1's MISO/SCK/MOSI and ARE on the
// header; they double as the ESP32-C6 link, which the shield takes over.
#define CART_SPI_PORT       spi1
#define PIN_CART_SCK        30
#define PIN_CART_MOSI       31
#define CART_SPI_BAUD       4000000

// Data bus D0..D7. Read only; never driven as outputs. Bit 6 swaps with the
// latch pin between variants -- see CART_LATCH_VARIANT above.
#if CART_LATCH_VARIANT
  #define PIN_CART_LATCH    8
  #define CART_DATA_PINS    { 7, 45, 41, 42, 44, 6, 43, 9 }
#else
  #define PIN_CART_LATCH    43
  #define CART_DATA_PINS    { 7, 45, 41, 42, 44, 6, 8, 9 }
#endif

// Chip selects for 0x8000 / 0xA000 / 0xC000, active low. 0xE000 lives on bit
// 15 of the shift register chain instead -- see cart_reader.cpp.
#define CART_CS_PINS        { 10, 20, 21 }

// How long to wait after asserting an address and chip select before sampling
// the data bus, in microseconds.
//
// It has to cover the 74HC595 propagation delay plus the ROM's access time.
// Period mask ROMs are typically 200-450 ns, but third-party and later
// cartridges can be considerably slower, and the shield adds its own delay.
//
// The failure is not obvious: bytes come back partly correct and decay towards
// zero rather than reading as garbage, so a cartridge looks unrecognised while
// the address bus and everything else test fine. If a particular cartridge is
// not detected but the probe shows the address bus working, raise this first.
#define CART_ACCESS_US      8

// Read every byte twice and require the two to agree, retrying a few times.
// Costs roughly double the dump time -- still under a second for 32 KB -- and
// turns a marginal read into a correct one rather than a silent corruption.
#define CART_VERIFY_READS   1

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

// ...except that pico_hdmi owns core 1 outright -- video_output_core1_run()
// never returns -- so the host has to live on core 0 there, whatever the
// setting above says.
//
// Forcing it here rather than special-casing each call site is what matters:
// every `#if !USB_HOST_ON_CORE1` guard in menu.cpp, fatal() and the emulator
// loop then services the stack automatically. Leaving it at 1 while the host
// actually ran on core 0 meant the menu never called tuh_task(), so nothing
// could enumerate: "USB: 0 devices, 0 HID, 0 pads".
#if VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
  #undef  USB_HOST_ON_CORE1
  #define USB_HOST_ON_CORE1 0
#endif

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
// ---------------------------------------------------------------------------
// Where emulator sound goes.
//
//   AUDIO_SINK_CODEC  TLV320DAC3100 -> headphone jack / speaker connector
//   AUDIO_SINK_HDMI   embedded in the HDMI stream as data islands
//
// HDMI audio is NOT implemented yet -- see the note in README.md. Selecting it
// raises a #error rather than building something silently mute, because the
// codec path works today and quietly losing it would be worse than not
// building. Flip the default to AUDIO_SINK_HDMI once the transport lands.
// ---------------------------------------------------------------------------
#define AUDIO_SINK_CODEC    0
#define AUDIO_SINK_HDMI     1

#define AUDIO_SINK          AUDIO_SINK_HDMI

// How many data island packets to keep queued. Each carries four samples, so
// 200 packets is 800 samples -- about 18 ms at 44.1 kHz. Enough to ride out a
// slow frame without adding noticeable latency.
#define HDMI_AUDIO_QUEUE_TARGET 200


// DMA channel PIO-USB transmits on. PIO_USB_DEFAULT_CONFIG hardcodes 0 without
// claiming it, so we reserve it and hand it over just before tuh_init().
//
// pico_hdmi hardcodes channels 0 AND 1 for its own ping-pong and claims them
// by name inside video_output_init(). dma_channel_claim() panics on a channel
// that is already taken, and a panic reads as a hard fault -- so with that
// driver PIO-USB has to sit elsewhere.
#if VIDEO_DRIVER == VIDEO_DRIVER_PICO_HDMI
  #define PIO_USB_TX_DMA_CH   2
#else
  #define PIO_USB_TX_DMA_CH   0
#endif

#if AUDIO_SINK == AUDIO_SINK_HDMI && VIDEO_DRIVER != VIDEO_DRIVER_PICO_HDMI
#error "AUDIO_SINK_HDMI needs VIDEO_DRIVER_PICO_HDMI: the built-in HSTX driver emits no data islands."
#endif

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

// Show a cartridge probe on the menu's status line in place of the controller
// count. Eight bytes:
//
//   first four   bank 0 at 0x0000, 0x0001, 0x1555, 0x1AAA
//   next three   banks 1, 2, 3 at offset 0
//   last         bank 0 at 0x0000 again
//
// Reading it:
//   first four all identical  the address is not reaching the cartridge --
//                             the shift registers are not latching, so try
//                             the other CART_LATCH_VARIANT
//   first four vary           address bus works; if there is still no AA 55
//                             or 55 AA the data bit order is wrong
//   everything 00             nothing driving the bus at all
//   a constant value, no cart  normal. With the slot empty the data bus
//                             floats, and RP2350 A2 erratum E9 lets a
//                             floating input latch high despite the internal
//                             pull-down -- 0xC0 is what an empty slot reads
//                             on at least one board. Harmless: it is not a
//                             valid header, so nothing is falsely detected.
//   last byte != first        readings unstable, suspect timing
// ---------------------------------------------------------------------------
// Boot diagnostics: the long-pulse build check, the numbered boot-progress
// blinks, and the per-call stage blinks inside video_init() and
// usb_host_init().
//
// These were how every bring-up problem in this project got located, and they
// are worth turning back on the moment something stops working. But they cost
// about 25 seconds of boot -- roughly 90% of it -- because each blink group
// deliberately pauses long enough to be counted.
//
// FATAL error codes are NOT affected. A failure still blinks its code forever,
// which is the one signal that must always be available.
// ---------------------------------------------------------------------------
#define BOOT_DIAGNOSTICS    0

// Stage blinks inside usb_host_init(). Requires BOOT_DIAGNOSTICS.
#define USB_HOST_STAGE_BLINK 0

// Show the emulated Z80's state in the border below the picture during play.
// Costs nothing visually -- the 256x192 image leaves rows 27-29 empty -- and
// answers what a "crash" actually is: a runaway PC, a HALT with interrupts
// disabled, or a stack that has wandered out of the 1 KB of RAM.
#define EMU_DEBUG_OVERLAY   0

#define SHOW_CART_DEBUG     0

#define SHOW_HID_DEBUG      0

// Build identifier, shown on the cartridge menu's title bar.
//
// Exists so "is the board actually running the code I just changed?" is
// answerable at a glance. A stale binary once produced button readings that
// perfectly matched an older decoder, and it took a full round of analysis to
// realise the source and the firmware had diverged. Bump this whenever you
// change something you intend to test.
#define ACJ_BUILD_ID        "build 64"


// ---------------------------------------------------------------------------
// USB clock test mode overrides. Placed last so they win over the settings
// above regardless of where those appear in this file.
// ---------------------------------------------------------------------------
#if USB_CLOCK_TEST_MODE
  #undef  ENABLE_VIDEO
  #define ENABLE_VIDEO      0         // no 126 MHz source in this mode
#endif
