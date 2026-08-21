# Fruit Jam pin usage

Source: https://learn.adafruit.com/adafruit-fruit-jam/pinout

## Used by this project

| Function | GPIO | Notes |
|---|---|---|
| HSTX CK- / CK+ | 12 / 13 | DVI clock lane |
| HSTX D0- / D0+ | 14 / 15 | DVI blue |
| HSTX D1- / D1+ | 16 / 17 | DVI green |
| HSTX D2- / D2+ | 18 / 19 | DVI red |
| I2C0 SDA / SCL | 20 / 21 | TLV320DAC3100 control, STEMMA QT |
| Peripheral reset | 22 | shared with ESP32-C6 — reset the C6 first |
| I2S IRQ | 23 | unused here |
| I2S DATA | 24 | |
| I2S MCLK | 25 | optional; we use BCLK as the PLL input |
| I2S BCLK | 26 | |
| I2S WS | 27 | |
| SD SCK | 34 | SPI0 |
| SD MOSI | 35 | SPI0 |
| SD MISO | 36 | SPI0 |
| SD CS | 39 | |
| USB host D+ | 1 | PIO-USB; D- must be D+ plus one |
| USB host D- | 2 | |
| USB host 5V enable | 11 | must be driven high to power the A ports |
| Button 1 | 0 | also BOOTSEL; held = reboot to bootloader |
| Button 2 | 4 | menu: down |
| Button 3 | 5 | menu: select |
| NeoPixels | 32 | not currently used |
| LED | 29 | active LOW; shared with the IR receiver |

## Free for the cartridge shield

The 2x16 header brings out: GPIO 6, 7, 8, 9, 10, 20, 21, 28, 30, 31, and
A1-A5 (GPIO 41-45). A0/GPIO40 is on the JST-PH connector only.

That is roughly 15 usable pins, which is why the cartridge reader needs shift
registers for the 15 address lines rather than driving them directly — the same
constraint the original Arduino ColecoVision reader worked around.

Note that GPIO 20/21 are the I2C bus used by the audio codec, and GPIO 8/9 are
the UART used for serial debug. If the shield needs those pins, either drop
serial debug or move the codec to software I2C.

## RP2350 A2 erratum E9

The Fruit Jam ships with the A2 die, which cannot reliably read a
high-impedance input pulled low by a weak resistor. Where the cartridge reader
needs a defined idle level it uses pull-*ups* and reads the cartridge's active
drive. If you add pull-downs anywhere, use 8.2k or smaller.
