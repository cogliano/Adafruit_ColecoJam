// audio.cpp -- I2S output to the Fruit Jam's TLV320DAC3100
//
// PIO drives a standard 32-bit-frame I2S master (BCLK + WS + DATA) and a DMA
// ping-pong feeds it. The codec is brought up over I2C0 at address 0x18.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include <string.h>

#include "audio_i2s.pio.h"

// Which PIO block carries I2S, selected by AUDIO_PIO_INSTANCE in config.h.
//
// PIO2 by default: Pico-PIO-USB uses PIO0 for its TX program and PIO1 for RX,
// so sharing PIO1 risks running out of state machines or instruction memory --
// and pio_add_program() panics outright when the memory is full.
#if   AUDIO_PIO_INSTANCE == 0
static PIO       i2s_pio = pio0;
#elif AUDIO_PIO_INSTANCE == 1
static PIO       i2s_pio = pio1;
#else
static PIO       i2s_pio = pio2;
#endif
static uint      i2s_sm;
static uint      i2s_offset;
static int       dma_ch_a, dma_ch_b;

// Two half-buffers of stereo 16-bit frames, packed as one 32-bit word per
// frame (left in the low half, right in the high half).
static uint32_t audio_buf[2][AUDIO_BUF_SAMPLES];
static volatile int  buf_ready = -1;      // index of the half needing a refill
static volatile bool audio_started = false;

// Diagnostics. audio_init() already knew whether the codec answered on I2C but
// threw the answer away, so a dead bus produced silence with no clue why.
static bool          codec_present = false;
static volatile uint32_t buffers_sent = 0;
static volatile uint32_t irq_count    = 0;
static uint8_t       codec_flags  = 0xFF;
static uint8_t       route_reg    = 0xFF;
static uint8_t       hpvol_reg    = 0xFF;

// ---------------------------------------------------------------------------
// TLV320DAC3100 register programming
// ---------------------------------------------------------------------------
static bool tlv_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_write_blocking(I2C_PORT, TLV320_I2C_ADDR, b, 2, false) == 2;
}

static uint8_t tlv_read(uint8_t reg) {
    uint8_t v = 0xFF;
    if (i2c_write_blocking(I2C_PORT, TLV320_I2C_ADDR, &reg, 1, true) != 1)
        return 0xFF;
    if (i2c_read_blocking(I2C_PORT, TLV320_I2C_ADDR, &v, 1, false) != 1)
        return 0xFF;
    return v;
}

static void tlv_page(uint8_t page) { tlv_write(0x00, page); }

static bool tlv_init(void) {
    // The DAC shares its reset line with the ESP32-C6. Pulse it low then high.
    gpio_init(PIN_PERIPH_RESET);
    gpio_set_dir(PIN_PERIPH_RESET, GPIO_OUT);
    gpio_put(PIN_PERIPH_RESET, 0);
    sleep_ms(10);
    gpio_put(PIN_PERIPH_RESET, 1);
    sleep_ms(50);

    i2c_init(I2C_PORT, I2C_BAUD);
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_SDA);
    gpio_pull_up(PIN_I2C_SCL);

    tlv_page(0);
    if (!tlv_write(0x01, 0x01)) return false;   // software reset
    sleep_ms(10);

    // Clock tree: BCLK is the PLL input (MCLK is optional on this board).
    // PLL_CLKIN = BCLK, CODEC_CLKIN = PLL_CLK
    tlv_write(0x04, 0x07);
    // PLL: P=1, R=1, J=64, D=0.
    //
    //   PLL_CLKIN = BCLK    = 32 x 44100      = 1.4112 MHz
    //   PLL_CLK   = J x in  = 64 x 1.4112 MHz = 90.3168 MHz
    //   fs = PLL_CLK / (NDAC x MDAC x DOSR)
    //      = 90.3168 MHz / (8 x 2 x 128)      = 44100 Hz exactly
    //
    // J was 32, which put PLL_CLK at 45.16 MHz. That is below the device's
    // specified 80-110 MHz PLL output range, so it could not lock -- no clock
    // reached the DAC and the part was silent regardless of everything else.
    // It would also have produced 22050 Hz had it locked.
    // Register 0x05: bit7 power up, bits 6:4 = P, bits 3:0 = R.
    // 0x92 -> powered, P=1 (encoded 001), R=2.
    //
    // R=2/J=32 rather than R=1/J=64: J is a SIX-BIT field with a valid range
    // of 4..63, so 64 does not fit and writing 0x40 lands outside it. Reaching
    // 90.3168 MHz has to be done with R, not by pushing J past its limit.
    tlv_write(0x05, 0x92);      // PLL power up, P=1, R=2
    tlv_write(0x06, 32);        // J = 32  -> 2 x 32 x 1.4112 = 90.3168 MHz
    tlv_write(0x07, 0x00);      // D MSB
    tlv_write(0x08, 0x00);      // D LSB
    sleep_ms(15);

    tlv_write(0x0B, 0x88);      // NDAC = 8, powered up
    tlv_write(0x0C, 0x82);      // MDAC = 2, powered up
    tlv_write(0x0D, 0x00);      // DOSR MSB
    tlv_write(0x0E, 0x80);      // DOSR = 128

    tlv_write(0x1B, 0x00);      // I2S, 16-bit, DAC is the clock slave
    // PRB_P1, not PRB_P11. The working reference sequence for this part uses
    // PRB_P1 at 44.1 kHz; PRB_P11 targets a different clock arrangement and
    // has its own MDAC/DOSR requirements, which ours may not satisfy.
    tlv_write(0x3C, 0x01);      // DAC processing block PRB_P1

    // Output routing: DAC -> headphone amp, and the class-D speaker amp.
    // DAC power-up and digital volume BEFORE the analog output stage, the
    // order the working reference uses. Configuring the output stage around an
    // already-running DAC is what its de-pop sequencing expects.
    tlv_page(0);
    tlv_write(0x3F, 0xD4);      // DAC L/R powered up, soft-step
    tlv_write(0x41, 0x00);      // DAC L digital volume 0 dB
    tlv_write(0x42, 0x00);      // DAC R digital volume 0 dB
    tlv_write(0x40, 0x00);      // unmute both DAC channels

    tlv_page(1);

    // This block mirrors a confirmed-working TLV320DAC3100 sequence rather
    // than my reading of the datasheet, which was wrong in three places at
    // once. Order matters: speaker down first, then headphone drivers up,
    // then routing, then volumes, then unmute the drivers.
    tlv_write(0x1F, 0xC4);      // power UP HPL and HPR, common mode 1.65 V

    // 0x20 = 0xC4, not 0x00. Empirically this board reports its headphone
    // drivers powered (DAC flag register bits 4 and 0) only when 0x20 holds
    // 0xC4; writing 0x00 here dropped them to unpowered while 0x1F still read
    // back 0xC4. Whatever this register is on this part, it gates HP power.
    tlv_write(0x20, 0xC4);
    tlv_write(0x2A, 0x01);      // mute speaker driver

    // NOT writing 0x1E. It is absent from Adafruit's own register map for this
    // device, and adding it is what coincided with losing HP driver power.

    tlv_write(0x21, 0x4E);      // de-pop timing

    tlv_write(0x23, TLV_HP_ROUTING);  // route DAC_L -> HPL, DAC_R -> HPR

    // D7 connects the output to the analog volume block; D6:D0 are
    // attenuation. See TLV_HP_VOLUME in config.h -- 0x70 leaves D7 clear and
    // silences the output, which is what the previous two builds did.
    tlv_write(0x24, TLV_HP_VOLUME);   // analog volume to HPL
    tlv_write(0x25, TLV_HP_VOLUME);   // analog volume to HPR

    tlv_write(0x28, 0x04);      // HPL driver unmuted, 0 dB
    tlv_write(0x29, 0x04);      // HPR driver unmuted, 0 dB

#if AUDIO_USE_SPEAKER
    // Speaker as well. Needs the board powered from 5 V, and Adafruit warn the
    // class-D amp adds noise to the headphone output when both are live.
    tlv_write(0x26, TLV_HP_VOLUME);   // analog volume to speaker
    tlv_write(0x2A, 0x1C);      // speaker driver unmuted, 12 dB
    tlv_write(0x20, 0xC6);      // power up the speaker amplifier
#endif


    // The output stage soft-steps its way up from mute. Adafruit's driver
    // waits 350 ms here; cutting it short means the first audio is inaudible
    // or badly attenuated.
    sleep_ms(350);

    return true;
}

void audio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    // Digital volume is -63.5 dB .. +24 dB in 0.5 dB steps, two's complement.
    int db_half = (percent == 0) ? -127 : (int)((percent - 100) * 0.6f);
    if (db_half < -127) db_half = -127;
    tlv_page(0);
    tlv_write(0x41, (uint8_t)(int8_t)db_half);
    tlv_write(0x42, (uint8_t)(int8_t)db_half);
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------
static void __isr audio_dma_handler(void) {
    // Both read_addr AND transfer_count have to be rewritten. A completed
    // channel's count reads back as zero, so re-triggering it via the chain
    // without restoring the count moves nothing and the ping-pong stops dead.
    // The video driver does restore both, which is why it keeps running; this
    // one only restored the address, and the audio stopped after a couple of
    // hundred buffers.
    if (dma_hw->ints1 & (1u << dma_ch_a)) {
        dma_hw->ints1 = 1u << dma_ch_a;
        dma_hw->ch[dma_ch_a].read_addr      = (uintptr_t)audio_buf[0];
        dma_hw->ch[dma_ch_a].transfer_count = AUDIO_BUF_SAMPLES;
        buf_ready = 0;
        irq_count++;
    }
    if (dma_hw->ints1 & (1u << dma_ch_b)) {
        dma_hw->ints1 = 1u << dma_ch_b;
        dma_hw->ch[dma_ch_b].read_addr      = (uintptr_t)audio_buf[1];
        dma_hw->ch[dma_ch_b].transfer_count = AUDIO_BUF_SAMPLES;
        buf_ready = 1;
        irq_count++;
    }
}

bool audio_init(void) {
    memset(audio_buf, 0, sizeof(audio_buf));

    // ORDER MATTERS: bring up the I2S clocks BEFORE configuring the codec.
    //
    // The TLV320's PLL is configured to use BCLK as its reference. Programming
    // it while the PIO is still idle asks it to lock onto a clock that does
    // not exist -- the part accepts the I2C writes (so the codec reports OK),
    // emits its power-up pop, and then sits mute with no valid clock. That pop
    // was the only sound the board ever made.
    //
    // Starting the state machine first means BCLK and LRCLK are already
    // running and stable when the PLL is told to lock to them.
    i2s_offset = pio_add_program(i2s_pio, &audio_i2s_program);
    i2s_sm     = pio_claim_unused_sm(i2s_pio, true);
    audio_i2s_program_init(i2s_pio, i2s_sm, i2s_offset,
                           PIN_I2S_DATA, PIN_I2S_BCLK);

    // BCLK runs at 32 x sample_rate (16-bit stereo frames) and the program
    // shifts one bit per two SM cycles, so the SM clock is 64 x sample_rate.
    //
    // Computed as a 24.8 fixed-point divider the same way pico-extras does it,
    // rather than via float: at 240 MHz and 44.1 kHz the exact ratio is
    // 85.0340..., and the fractional part matters -- rounding it away detunes
    // the output by a noticeable amount.
    const uint32_t divider =
        (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 4) / AUDIO_SAMPLE_RATE);
    pio_sm_set_clkdiv_int_frac(i2s_pio, i2s_sm,
                               (uint16_t)(divider >> 8), (uint8_t)(divider & 0xFF));

    // Clocks first. The SM shifts a stale OSR until real data arrives, which
    // is silence, but BCLK and LRCLK are what the codec's PLL needs.
    pio_sm_set_enabled(i2s_pio, i2s_sm, true);
    sleep_ms(10);                   // let the clocks settle

    // Now the codec, with a live reference to lock to.
    bool codec_ok = tlv_init();
    codec_present = codec_ok;
    sleep_ms(20);                   // allow the PLL to acquire lock

    // Read the DAC flag register back rather than assuming. Bit 7 = left DAC
    // powered up, bit 3 = right DAC powered up. Those bits only set when the
    // part has a valid clock, so this distinguishes "configured" from
    // "actually running" -- which is precisely what we could not tell before.
    tlv_page(0);
    codec_flags = tlv_read(0x25);

    // Read back the two page-1 registers that decide whether the DAC actually
    // reaches the headphone amplifier. Writing them is not the same as them
    // holding the value -- a mis-paged write goes somewhere else entirely and
    // reads back as whatever that register happens to contain.
    tlv_page(1);
    route_reg = tlv_read(0x23);     // output mixer routing, expect 44
    hpvol_reg = tlv_read(0x24);     // analog vol to HPL, expect 80 (D7 set)
    tlv_page(0);

#if AUDIO_CPU_FEED
    // No DMA at all in this mode: core 1 pushes samples directly. The state
    // machine is already running from above.
    audio_started = true;
    return codec_ok;
#else
    dma_ch_a = dma_claim_unused_channel(true);
    dma_ch_b = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(dma_ch_a);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&c, dma_ch_b);
    dma_channel_configure(dma_ch_a, &c, &i2s_pio->txf[i2s_sm],
                          audio_buf[0], AUDIO_BUF_SAMPLES, false);

    c = dma_channel_get_default_config(dma_ch_b);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&c, dma_ch_a);
    dma_channel_configure(dma_ch_b, &c, &i2s_pio->txf[i2s_sm],
                          audio_buf[1], AUDIO_BUF_SAMPLES, false);

    dma_hw->ints1 = (1u << dma_ch_a) | (1u << dma_ch_b);
    dma_hw->inte1 = (1u << dma_ch_a) | (1u << dma_ch_b);
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    // The state machine is already enabled -- do not restart it here, that
    // would reset its shift state out from under the codec's PLL.
    dma_channel_start(dma_ch_a);
    audio_started = true;
#endif

    return codec_ok;
}

// Push samples straight into the state machine, no DMA involved. Each 32-bit
// word is one stereo frame with the same sample in both halves.
//
// Blocking on purpose: if the SM is not pulling, this wedges core 1 and the
// menu stops responding to the pad -- an unmistakable signal, and far more
// useful than silently doing nothing.
void audio_push_blocking(const int16_t *mono, int count) {
    for (int i = 0; i < count; i++) {
        const uint16_t v = (uint16_t)mono[i];
        pio_sm_put_blocking(i2s_pio, i2s_sm, ((uint32_t)v << 16) | v);
    }
    buffers_sent++;
}

int audio_buffer_needed(void) {
    int b = buf_ready;
    buf_ready = -1;
    return b;
}

bool     audio_codec_ok(void)     { return codec_present; }
uint8_t  audio_codec_flags(void)  { return codec_flags; }
uint8_t  audio_route_reg(void)    { return route_reg; }
uint8_t  audio_hpvol_reg(void)    { return hpvol_reg; }
uint32_t audio_buffers_sent(void)  { return buffers_sent; }
uint32_t audio_irq_count(void)     { return irq_count; }

bool audio_i2s_running(void) {
    return audio_started && !pio_sm_is_tx_fifo_full(i2s_pio, i2s_sm);
}

// Live state of the I2S path, for on-screen diagnosis.
//   pc   program counter of the state machine. If this never changes between
//        redraws the SM is not executing at all.
//   fifo TX FIFO level, 0-8. Stuck at 8 means the SM is not draining it.
//   busy bit1 = DMA channel A busy, bit0 = channel B busy. Both clear with a
//        full FIFO means the DMA has nothing to push into and has stopped.
void audio_debug(uint8_t *pc, uint8_t *fifo, uint8_t *busy) {
    *pc   = (uint8_t)pio_sm_get_pc(i2s_pio, i2s_sm);
    *fifo = (uint8_t)pio_sm_get_tx_fifo_level(i2s_pio, i2s_sm);
#if AUDIO_CPU_FEED
    *busy = 0;
#else
    *busy = (uint8_t)((dma_channel_is_busy(dma_ch_a) ? 2 : 0) |
                      (dma_channel_is_busy(dma_ch_b) ? 1 : 0));
#endif
}

void audio_submit(int half, const int16_t *mono, int count) {
    if (half < 0 || half > 1) return;
    buffers_sent++;
    uint32_t *dst = audio_buf[half];
    int n = (count < AUDIO_BUF_SAMPLES) ? count : AUDIO_BUF_SAMPLES;
    for (int i = 0; i < n; i++) {
        uint16_t s = (uint16_t)mono[i];
        dst[i] = ((uint32_t)s << 16) | s;      // same sample to both channels
    }
    for (int i = n; i < AUDIO_BUF_SAMPLES; i++) dst[i] = 0;
}
