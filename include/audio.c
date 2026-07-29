/*****************************************************************
Ultimate 64 Audio Hardware Layer — implementation
See audio.h for API documentation.
******************************************************************/

#include <c64/reu.h>
#include "audio.h"

// ---------------------------------------------------------------
// Channel base address table
// ---------------------------------------------------------------
const unsigned audio_ch_base[AUDIO_NUM_CHANNELS] = {
    AUDIO_CH0, AUDIO_CH1, AUDIO_CH2, AUDIO_CH3,
    AUDIO_CH4, AUDIO_CH5, AUDIO_CH6
};

// ---------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------

// Write a byte to a channel register
static void ch_wr(char ch, char offset, unsigned char val) {
    volatile unsigned char *p =
        (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
    p[offset] = val;
}

// Read a byte from a channel register
static unsigned char ch_rd(char ch, char offset) {
    volatile unsigned char *p =
        (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
    return p[offset];
}

// Write audsms: [0x01=SDRAM bank][addr_hi][addr_mid][addr_lo]
// The UA uses absolute 32-bit SDRAM addresses. REU maps to SDRAM at $01000000
// (base = $01×16MB), so REU address $6C3C → SDRAM $01006C3C → [$01][$00][$6C][$3C].
// Reference: ModPlayer_16k buffer.asm writes #$01 at audsms+0, then smptrh/m/l.
// This was tested with partially-loaded REU before; now REU is fully loaded.
static void ch_wr_sms(char ch, unsigned long addr) {
    volatile unsigned char *p =
        (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
    p[AUDIO_OFF_SMS + 0] = 0x01;                          // SDRAM bank ($01×16MB = REU base)
    p[AUDIO_OFF_SMS + 1] = (unsigned char)(addr >> 16);   // addr hi  (bits 16-23)
    p[AUDIO_OFF_SMS + 2] = (unsigned char)(addr >> 8);    // addr mid (bits 8-15)
    p[AUDIO_OFF_SMS + 3] = (unsigned char)(addr);         // addr lo  (bits 0-7)
}

// Write a 24-bit value MSB-first (big-endian) starting at offset.
// Used for audsml (length) and audrpa/rpb (loop point addresses).
// Reference: ModPlayer_16k writes samplelen/rep as [hi][mid][lo].
static void ch_wr_be24(char ch, char offset, unsigned long val) {
    volatile unsigned char *p =
        (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
    p[offset + 0] = (unsigned char)(val >> 16);   // hi
    p[offset + 1] = (unsigned char)(val >> 8);    // mid
    p[offset + 2] = (unsigned char)(val);         // lo
}

// Write a 16-bit rate value MSB-first (big-endian).
// audrat: byte 0 ($DF2E) = hi, byte 1 ($DF2F) = lo.
// Reference: ModPlayer_16k buffer.asm stores PRODUCT+3 (MSB) at audrat,
// PRODUCT+2 (LSB) at audrat+1 — confirmed big-endian from periodfix.asm analysis.
static void ch_wr16(char ch, char offset, unsigned val) {
    volatile unsigned char *p =
        (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
    p[offset + 0] = (unsigned char)(val >> 8);   // hi byte → $DF2E
    p[offset + 1] = (unsigned char)(val);         // lo byte → $DF2F
}

// ---------------------------------------------------------------
// audio_reset
// ---------------------------------------------------------------
void audio_reset(void) {
    char ch, i;
    for (ch = 0; ch < AUDIO_NUM_CHANNELS; ch++) {
        volatile unsigned char *p =
            (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
        for (i = 0x1E; i >= 0; i--)
            p[i] = 0;
    }
}

// ---------------------------------------------------------------
// audio_detect
//
// Direct C translation of the ModPlayer_16k detectaudio routine
// by 6510nl (audio.asm).
//
// Steps (matching the reference exactly):
//  1. Stop all 7 channels (write 0 to audctr).
//  2. Ack IRQ on ch0 only (write 0xFF to audirq).
//  3. Read audist 256 times — must stay exactly 0; any non-zero fails.
//  4. Play 256-byte looping sample: start=0, vol=0, length=256,
//     rate=256, CTR=0x05 (start+loop).
//  5. Wait up to 128 reads for audist to go non-zero.
//  6. Verify audist == 0x01 for the remaining count (0 → 256 reads).
//  7. Ack IRQ; return 1 (found).
// ---------------------------------------------------------------
char audio_detect(void) {
    volatile unsigned char *ch0 =
        (volatile unsigned char *)(unsigned long)AUDIO_CH0;
    unsigned char i;
    char ch;

    __asm { sei }

    // Stop all 7 channels
    for (ch = 0; ch < AUDIO_NUM_CHANNELS; ch++) {
        volatile unsigned char *p =
            (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
        p[AUDIO_OFF_CTR] = 0;
    }
    // Ack IRQ on ch0 only
    ch0[AUDIO_OFF_IRQ] = 0xFF;

    // audist must read exactly 0 for 256 consecutive reads
    i = 0;
    do {
        if (ch0[AUDIO_OFF_STATUS] != 0) {
            __asm { cli }
            return 0;
        }
    } while (--i != 0);

    // Start minimal looping sample: start=0, vol=0, length=256, rate=256
    // audsms: [lo=0][mid=0][hi=0][bank=0x01] = little-endian $01000000 = REU $000000
    // audsml: big-endian 256 = [hi=0x00][mid=0x01][lo=0x00]
    ch0[AUDIO_OFF_VOL]     = 0;
    ch0[AUDIO_OFF_SMS + 0] = 0x01; // SDRAM bank $01 (REU base $01000000)
    ch0[AUDIO_OFF_SMS + 1] = 0;    // addr hi  = 0
    ch0[AUDIO_OFF_SMS + 2] = 0;    // addr mid = 0
    ch0[AUDIO_OFF_SMS + 3] = 0;    // addr lo  = 0
    ch0[AUDIO_OFF_RAT + 0] = 0;    // rate hi = 0 → rate = 1 (big-endian: fast loop for detection)
    ch0[AUDIO_OFF_SML + 0] = 0;    // length hi  = 0
    ch0[AUDIO_OFF_RAT + 1] = 1;    // rate lo = 1
    ch0[AUDIO_OFF_SML + 1] = 1;    // length mid = 1  → 256 bytes big-endian
    ch0[AUDIO_OFF_SML + 2] = 0;    // length lo  = 0
    ch0[AUDIO_OFF_CTR]     = AUDIO_CTR_START | AUDIO_CTR_RESTART; // $05: restart-from-start loop

    // Wait up to 128 reads for audist to become non-zero
    i = 0x80;
    do {
        if (ch0[AUDIO_OFF_STATUS] != 0)
            break;
    } while (--i != 0);

    // Verify audist == 0x01 for remaining i reads (i=0 → 256 reads)
    do {
        if (ch0[AUDIO_OFF_STATUS] != AUDIO_ST_IRQ) {
            __asm { cli }
            return 0;
        }
    } while (--i != 0);

    ch0[AUDIO_OFF_IRQ] = 0xFF;

    __asm { cli }
    return 1;
}

// ---------------------------------------------------------------
// audio_get_version
// ---------------------------------------------------------------
unsigned char audio_get_version(void) {
    return ch_rd(0, AUDIO_OFF_VERSION);
}

// ---------------------------------------------------------------
// audio_channel_stop
// ---------------------------------------------------------------
void audio_channel_stop(char ch) {
    ch_wr(ch, AUDIO_OFF_CTR, AUDIO_CTR_STOP);
}

// ---------------------------------------------------------------
// audio_channel_play  (one-shot)
// ---------------------------------------------------------------
void audio_channel_play(char ch,
                        unsigned long start,
                        unsigned long length,
                        unsigned     rate,
                        unsigned char vol,
                        unsigned char pan) {
    if (vol > AUDIO_VOLUME_MAX) vol = AUDIO_VOLUME_MAX;
    ch_wr(ch, AUDIO_OFF_CTR, AUDIO_CTR_STOP);
    ch_wr(ch, AUDIO_OFF_VOL, vol);
    ch_wr(ch, AUDIO_OFF_PAN, pan);
    ch_wr_sms(ch, start);
    ch_wr_be24(ch, AUDIO_OFF_SML, length);
    ch_wr16(ch, AUDIO_OFF_RAT, rate);
    ch_wr(ch, AUDIO_OFF_CTR, AUDIO_CTR_START);
}

// ---------------------------------------------------------------
// audio_channel_loop
// ---------------------------------------------------------------
void audio_channel_loop(char ch,
                        unsigned long start,
                        unsigned long length,
                        unsigned long loop_a,
                        unsigned long loop_b,
                        unsigned     rate,
                        unsigned char vol,
                        unsigned char pan) {
    if (vol > AUDIO_VOLUME_MAX) vol = AUDIO_VOLUME_MAX;
    ch_wr(ch, AUDIO_OFF_CTR, AUDIO_CTR_STOP);
    ch_wr(ch, AUDIO_OFF_VOL, vol);
    ch_wr(ch, AUDIO_OFF_PAN, pan);
    ch_wr_sms(ch, start);
    ch_wr_be24(ch, AUDIO_OFF_SML, length);
    ch_wr16(ch, AUDIO_OFF_RAT, rate);
    ch_wr_be24(ch, AUDIO_OFF_RPA, loop_a);
    ch_wr_be24(ch, AUDIO_OFF_RPB, loop_b);
    ch_wr(ch, AUDIO_OFF_CTR, AUDIO_CTR_START | AUDIO_CTR_LOOP);
}

// ---------------------------------------------------------------
// audio_channel_set_*
// ---------------------------------------------------------------
void audio_channel_set_volume(char ch, unsigned char vol) {
    if (vol > AUDIO_VOLUME_MAX) vol = AUDIO_VOLUME_MAX;
    ch_wr(ch, AUDIO_OFF_VOL, vol);
}
void audio_channel_set_pan(char ch, unsigned char pan) {
    ch_wr(ch, AUDIO_OFF_PAN, pan);
}
void audio_channel_set_rate(char ch, unsigned rate) {
    ch_wr16(ch, AUDIO_OFF_RAT, rate);
}
void audio_channel_ack_irq(char ch) {
    ch_wr(ch, AUDIO_OFF_IRQ, 0xFF);
}

// ---------------------------------------------------------------
// reu_fetch — wrapper over Oscar64's native reu_load
// ---------------------------------------------------------------
void reu_fetch(void *c64dest, unsigned long reu_src, unsigned len) {
    reu_load(reu_src, (volatile char *)c64dest, len);
}
