/*****************************************************************
Ultimate 64 Audio Hardware Layer

Direct register access to the Ultimate Audio DMA module.
Up to 7 simultaneous voices; 8-bit PCM from REU/SDRAM.

Firmware prerequisite: "C64 and cartridge settings" menu must
have the Ultimate Audio module mapped at $DF20.
Audio register space: $DF20–$DFFF.
REU (sample storage) register space: $DF00–$DF0B.
******************************************************************/

#ifndef _AUDIO_H_
#define _AUDIO_H_

// ---------------------------------------------------------------
// Channel base addresses
// ---------------------------------------------------------------
#define AUDIO_CH0   0xDF20
#define AUDIO_CH1   0xDF40
#define AUDIO_CH2   0xDF60
#define AUDIO_CH3   0xDF80
#define AUDIO_CH4   0xDFA0
#define AUDIO_CH5   0xDFC0
#define AUDIO_CH6   0xDFE0

#define AUDIO_NUM_CHANNELS  7

// Convenience table for base address by channel index (0–6)
// Access: audio_ch_base[n]
extern const unsigned audio_ch_base[AUDIO_NUM_CHANNELS];

// ---------------------------------------------------------------
// Register offsets (applied to any channel base address)
//
// Read:
#define AUDIO_OFF_STATUS  0x00  // 8-bit  Interrupt status (read)
#define AUDIO_OFF_VERSION 0x01  // 8-bit  Module version  (read)
// Write:
#define AUDIO_OFF_CTR     0x00  // 8-bit  Control
#define AUDIO_OFF_VOL     0x01  // 8-bit  Volume  (0=silent, 63=max — hardware register is 6-bit)
#define AUDIO_OFF_PAN     0x02  // 8-bit  Panning (0=left, 128=centre, 255=right)
#define AUDIO_OFF_SMS     0x04  // 32-bit Sample start address (LSB first, 4 bytes)
#define AUDIO_OFF_SML     0x09  // 24-bit Sample length       (LSB first, 3 bytes)
#define AUDIO_OFF_RAT     0x0E  // 16-bit Playback rate/period (LSB first, 2 bytes)
#define AUDIO_OFF_RPA     0x11  // 24-bit Repeat point A      (LSB first, 3 bytes)
#define AUDIO_OFF_RPB     0x15  // 24-bit Repeat point B      (LSB first, 3 bytes)
#define AUDIO_OFF_IRQ     0x1F  // 8-bit  IRQ settings / acknowledge

// ---------------------------------------------------------------
// Control register bits (AUDIO_OFF_CTR / write to $xx20/$xx40…)
// ---------------------------------------------------------------
#define AUDIO_CTR_START   0x01  // Bit 0: start/enable voice
#define AUDIO_CTR_LOOP    0x02  // Bit 1: loop between RPA and RPB
#define AUDIO_CTR_RESTART 0x04  // Bit 2: restart-from-start loop (used in detection)
#define AUDIO_CTR_STOP    0x00  // Write 0 to stop voice

// Interrupt status register (AUDIO_OFF_STATUS / read)
#define AUDIO_ST_IRQ      0x01  // Bit 0: interrupt pending

// Rate formula: rate = round(6_250_000 / sample_rate_Hz)
// Sample rate formula: Hz = 6_250_000 / rate
// Amiga→UA: rate = (unsigned long)amiga_period * 12500 / 7094

// UA volume register is 6-bit: hardware silently ignores bits 6–7.
// Writing values > 63 produces inconsistent loudness across samples.
#define AUDIO_VOLUME_MAX  63

// ---------------------------------------------------------------
// Panning presets
// ---------------------------------------------------------------
#define AUDIO_PAN_LEFT    0x00
#define AUDIO_PAN_CENTRE  0x80
#define AUDIO_PAN_RIGHT   0xFF

// ---------------------------------------------------------------
// REU (RAM Expansion Unit) registers — shared $DF00 space
// ---------------------------------------------------------------
#define REU_STATUS   (*(volatile unsigned char *)0xDF00)
#define REU_CMD      (*(volatile unsigned char *)0xDF01)
#define REU_C64LO    (*(volatile unsigned char *)0xDF02)
#define REU_C64HI    (*(volatile unsigned char *)0xDF03)
#define REU_REULO    (*(volatile unsigned char *)0xDF04)
#define REU_REUMI    (*(volatile unsigned char *)0xDF05)
#define REU_REUHI    (*(volatile unsigned char *)0xDF06)
#define REU_LENLO    (*(volatile unsigned char *)0xDF07)
#define REU_LENHI    (*(volatile unsigned char *)0xDF08)
#define REU_CONTROL  (*(volatile unsigned char *)0xDF0A)

#define REU_CMD_FETCH  0xD1   // REU → C64 RAM

// ---------------------------------------------------------------
// Function prototypes
// ---------------------------------------------------------------

char audio_detect(void);
// Returns 1 if the Ultimate Audio module is present and responds
// correctly; 0 if not detected.  Also resets all channels.

unsigned char audio_get_version(void);
// Returns the module version byte from channel 0. Only valid
// after audio_detect() returns 1.

void audio_reset(void);
// Zero all registers across all 7 channels.

void audio_channel_stop(char ch);
// Immediately stop playback on channel ch (0–6).

void audio_channel_play(char ch,
                        unsigned long start,
                        unsigned long length,
                        unsigned     rate,
                        unsigned char vol,
                        unsigned char pan);
// One-shot playback.
//   ch     : channel index 0–6
//   start  : 32-bit REU address of first sample byte
//   length : 24-bit sample byte count
//   rate   : playback rate (see formula above)
//   vol    : 0–255
//   pan    : 0 (left) … 128 (centre) … 255 (right)

void audio_channel_loop(char ch,
                        unsigned long start,
                        unsigned long length,
                        unsigned long loop_a,
                        unsigned long loop_b,
                        unsigned     rate,
                        unsigned char vol,
                        unsigned char pan);
// Looping playback between loop_a and loop_b.


void audio_channel_set_volume(char ch, unsigned char vol);
void audio_channel_set_pan(char ch, unsigned char pan);
void audio_channel_set_rate(char ch, unsigned rate);

void audio_channel_ack_irq(char ch);
// Acknowledge a channel's end-of-sample IRQ ($1F write 0xFF).

void reu_fetch(void *c64dest, unsigned long reu_src, unsigned len);
// Transfer `len` bytes from REU address `reu_src` into C64 RAM
// at `c64dest` using synchronous DMA.

#pragma compile("audio.c")

#endif
