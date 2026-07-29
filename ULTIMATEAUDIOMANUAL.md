# Ultimate Audio Library Manual

**Ultimate 64 Audio DMA Layer and ProTracker MOD Player for Oscar64**

Library files:
- `include/audio.h` / `include/audio.c` — hardware layer
- `include/modplay.h` / `include/modplay.c` — MOD player engine

---

## Contents

1. [Overview](#1-overview)
2. [Hardware Architecture](#2-hardware-architecture)
3. [Firmware Prerequisites](#3-firmware-prerequisites)
4. [Audio Hardware Layer](#4-audio-hardware-layer)
5. [MOD Player — Concepts](#5-mod-player--concepts)
6. [MOD Player — API Reference](#6-mod-player--api-reference)
7. [MOD Player — Effects Reference](#7-mod-player--effects-reference)
8. [Typical Usage Patterns](#8-typical-usage-patterns)
9. [Memory Layout and Constraints](#9-memory-layout-and-constraints)
10. [Timing and IRQ Details](#10-timing-and-irq-details)
11. [Limitations and Known Issues](#11-limitations-and-known-issues)

---

## 1. Overview

The library provides two layers:

**Audio hardware layer** (`audio.h`) — direct register access to the Ultimate Audio DMA module. Seven independent voices, each playing 8-bit unsigned PCM data from REU/SDRAM at any sample rate derived from a 6.25 MHz reference clock. Supports one-shot and looping playback with configurable volume and stereo panning.

**MOD player engine** (`modplay.h`) — a full ProTracker-compatible 4-channel MOD file player. MOD files are loaded into REU via the Ultimate Command Interface and then played entirely in the background via a CIA1 timer IRQ. The CPU is free for demo effects between ticks. All standard ProTracker effects are implemented.

---

## 2. Hardware Architecture

### Ultimate Audio channels

The Ultimate Audio module maps seven DMA voices into the C64's I/O space at `$DF20`–`$DFFF`. Each voice has its own 32-bit sample start address, 24-bit length, 16-bit playback rate, 24-bit loop points A and B, 8-bit volume, and 8-bit panning register.

| Channel | Base address |
|---------|-------------|
| 0 | `$DF20` |
| 1 | `$DF40` |
| 2 | `$DF60` |
| 3 | `$DF80` |
| 4 | `$DFA0` |
| 5 | `$DFC0` |
| 6 | `$DFE0` |

The MOD player uses **channels 0–3** for music. **Channels 4–6** are free for sound effects.

### Register offsets per channel

| Offset | Width | Dir | Name | Description |
|--------|-------|-----|------|-------------|
| `$00` | 8 | R | Status | Bit 0 = end-of-sample IRQ pending |
| `$01` | 8 | R | Version | Module firmware version |
| `$00` | 8 | W | Control | Start/stop/loop bits |
| `$01` | 8 | W | Volume | 0 = silent, 63 = maximum (6-bit register; values > 63 cause inconsistent loudness) |
| `$02` | 8 | W | Pan | 0 = full left, 128 = centre, 255 = full right |
| `$04` | 32 | W | Start | Sample start address in REU (LSB first) |
| `$09` | 24 | W | Length | Sample byte count (LSB first) |
| `$0E` | 16 | W | Rate | Playback rate (LSB first) |
| `$11` | 24 | W | Loop A | Loop start byte offset (LSB first) |
| `$15` | 24 | W | Loop B | Loop end byte offset (LSB first) |
| `$1F` | 8 | W | IRQ | Write `$FF` to acknowledge end-of-sample IRQ |

### Control register bits

| Bit | Constant | Value | Meaning |
|-----|----------|-------|---------|
| 0 | `AUDIO_CTR_START` | `$01` | Start/enable voice |
| 1 | `AUDIO_CTR_LOOP` | `$02` | Loop continuously between Loop A and Loop B |
| 2 | `AUDIO_CTR_RESTART` | `$04` | Restart-from-start loop (used internally by `audio_detect()`) |
| — | `AUDIO_CTR_STOP` | `$00` | Stop voice |

Always write `AUDIO_CTR_STOP` (`$00`) before reconfiguring a voice, then write the start value.

### Rate formula

```
rate = round(6_250_000 / playback_Hz)
playback_Hz = 6_250_000 / rate
```

**Amiga period → UA rate (PAL Amiga reference):**
```
rate = (unsigned long)amiga_period * 12_500 / 7094
```

For standard A-440 tuning (Amiga period 428):
`rate = 428 × 12500 / 7094 = 754`

### REU DMA access

Sample data sits in the C64's REU (RAM Expansion Unit). The Ultimate 64 provides 16 MB of SDRAM accessible as REU at addresses `$000000`–`$FFFFFF`. The REU DMA registers at `$DF00`–`$DF0A` transfer blocks of data between C64 RAM and REU synchronously (CPU stalls during the DMA).

```
reu_fetch(c64_dest_ptr, reu_src_address, byte_count);
```

---

## 3. Firmware Prerequisites

### Ultimate Audio mapping

Path: **F2 → C64 and cartridge settings → enable Ultimate Audio**

The audio module must be mapped to `$DF20`. If it is not mapped, `audio_detect()` returns 0.

### REU

The U64 provides a built-in 16 MB REU. No separate REU module is required. The REU registers are at `$DF00`.

**Note:** The audio module and REU share the `$DF00`–`$DFFF` range. The audio module uses `$DF20`–`$DFFF`; REU DMA registers use `$DF00`–`$DF0A`. They coexist without conflict.

### UCI (for loading MOD files)

`modplay_load()` uses the UCI library to fetch a file from Ultimate storage directly into REU. The UCI registers must be accessible at `$DF1C` (requires `uii_detect()` to return 1 beforehand).

---

## 4. Audio Hardware Layer

### `audio_detect`

```c
char audio_detect(void);
```

Tests whether the Ultimate Audio module is present by:
1. Stopping all voices and clearing IRQ flags.
2. Reading the status register 256 times — must remain 0.
3. Triggering a minimal 1-byte silent sample and waiting for the end-of-sample IRQ to fire.
4. Verifying the IRQ status holds consistently.

Returns **1** if detected, **0** if absent. Also calls `audio_reset()` on success.

### `audio_get_version`

```c
unsigned char audio_get_version(void);
```

Returns the module version byte from channel 0's version register (`$DF21`). Only meaningful after `audio_detect()` has returned 1; undefined if the module is absent.

---

### `audio_reset`

```c
void audio_reset(void);
```

Writes zero to all registers across all 7 channels. Safe to call at any time.

### `audio_channel_play`

```c
void audio_channel_play(char ch,
                        unsigned long start,
                        unsigned long length,
                        unsigned     rate,
                        unsigned char vol,
                        unsigned char pan);
```

One-shot playback. The voice plays the sample once and stops automatically at end-of-sample (IRQ flag set in status register).

| Parameter | Description |
|-----------|-------------|
| `ch` | Channel 0–6 |
| `start` | 32-bit REU address of first sample byte |
| `length` | 24-bit byte count |
| `rate` | Playback rate (see formula above) |
| `vol` | 0–63 (clamped to `AUDIO_VOLUME_MAX`) |
| `pan` | 0 (left) … 128 (centre) … 255 (right) |

### `audio_channel_loop`

```c
void audio_channel_loop(char ch,
                        unsigned long start,
                        unsigned long length,
                        unsigned long loop_a,
                        unsigned long loop_b,
                        unsigned     rate,
                        unsigned char vol,
                        unsigned char pan);
```

Looping playback. The sample plays from `start` to `start + length`, then continuously loops between `loop_a` and `loop_b` (both are absolute REU addresses). Loop continues until `audio_channel_stop()` is called.

### `audio_channel_stop`

```c
void audio_channel_stop(char ch);
```

Immediately silences and stops channel `ch`.

### Setters (in-flight updates)

```c
void audio_channel_set_volume(char ch, unsigned char vol);
void audio_channel_set_pan(char ch, unsigned char pan);
void audio_channel_set_rate(char ch, unsigned rate);
```

These write directly to the UA registers and take effect on the next DMA fetch cycle. They are safe to call while the voice is playing — the DMA engine reads the new rate on the next sample fetch, producing a smooth pitch change.

### `audio_channel_ack_irq`

```c
void audio_channel_ack_irq(char ch);
```

Clears the end-of-sample IRQ flag by writing `$FF` to offset `$1F`. Call this if you are polling the status register for end-of-sample detection in non-IRQ code.

### `reu_fetch`

```c
void reu_fetch(void *c64dest, unsigned long reu_src, unsigned len);
```

Synchronous REU DMA: transfers `len` bytes from REU address `reu_src` into C64 RAM at `c64dest`. The CPU stalls during the transfer (no busy-wait needed). Maximum safe transfer in one call: 65535 bytes. Transfers must not cross a 512 KB REU bank boundary on some REU implementations; the Ultimate 64's built-in REU does not have this restriction.

---

## 5. MOD Player — Concepts

### MOD file format

A ProTracker MOD file (4-channel, 31-sample `.MOD`) has the structure:

```
Offset    Size   Content
$000000   20     Song title
$000014   930    31 × 30 bytes sample info (name, length, finetune, volume, loops)
$0003AE   1      Song length (number of order entries, 1–128)
$0003AF   1      Padding (usually 127)
$0003B0   128    Order table (pattern sequence, one byte per entry)
$000430   4      Format ID: "M.K." / "4CHN" / "FLT4" / "M!K!"
$000434   var    Pattern data: num_patterns × 64 rows × 16 bytes per row
$000434   var    Sample data: raw 8-bit signed PCM, concatenated
  + patterns
```

Each 16-byte row contains 4 note cells of 4 bytes each:

```
Byte 0: [sample_hi 4 bits][period_hi 4 bits]
Byte 1: period_lo
Byte 2: [sample_lo 4 bits][effect_cmd 4 bits]
Byte 3: effect_param
```

### Loading workflow

```
1. uii_detect()           → verify UCI present
2. audio_detect()         → verify Ultimate Audio present
3. uii_change_dir(path)   → navigate to MOD file
4. modplay_load(name, reu_addr)  → fetch file into REU
5. modplay_init(reu_addr) → parse header, build sample table
6. modplay_start()        → install IRQ, begin playback
```

### Sample storage in REU

`modplay_init()` computes the exact REU address of each sample's PCM data based on the MOD header. During playback, `modplay_tick()` passes these addresses directly to the Ultimate Audio start/loop registers. No additional copying is required.

### ProTracker timing

The default tempo is **125 BPM** with **6 ticks per row**.

```
ticks_per_second = BPM × 2 / 5
At BPM 125: 50 ticks/second (PAL)

CIA1 timer A value = 2_463_120 / BPM   (PAL, ~985 kHz CIA clock)
At BPM 125: timer = 19705
```

The `Fxx` effect changes BPM (if `xx ≥ 32`) or ticks-per-row (if `xx < 32`). BPM changes reprogram the CIA timer at the start of the next tick 0.

---

## 6. MOD Player — API Reference

### `modplay_load`

```c
char modplay_load(char *filename, unsigned long reu_addr);
```

Load a MOD file from Ultimate filesystem storage into REU.

Opens the file, queries its size with `uii_file_size()`, then transfers the entire file to REU at `reu_addr` via `uii_load_reu(reu_addr, size)`.

| Parameter | Description |
|-----------|-------------|
| `filename` | File name (null-terminated); directory must be pre-selected with `uii_change_dir()` |
| `reu_addr` | Destination REU base address (e.g. `0x010000`) |

**Returns:** 1 on success, 0 on UCI error or empty file (check `uii_status` for details).

**Prerequisites:** `uii_detect()` must have returned 1. The UCI DOS library must be included.

**Note:** The function uses `uii_load_reu(reu_addr, size)` from `ultimate_dos_lib`, which loads an already-open file into REU at an explicit address with an explicit byte count. This replaced an earlier size-index–based API.

**Example:**
```c
uii_change_dir("/usb0/music/");
if (!modplay_load("demotrack.mod", 0x010000)) {
    // load failed
}
```

### `modplay_init`

```c
char modplay_init(unsigned long reu_addr);
```

Parse the MOD file at `reu_addr` in REU. Reads the 1084-byte header into C64 RAM, validates the format, builds the sample address table, and initialises all player state.

**Returns:** 1 if format is recognised, 0 if unrecognised.

Supported formats: `M.K.`, `M!K!`, `4CHN`, `FLT4` (31 samples), and old-style 15-sample files.

**Does not start playback** — call `modplay_start()` after this.

### `modplay_start`

```c
void modplay_start(void);
```

Begin playback from order 0, row 0. Installs the CIA1 timer A IRQ handler at `$0314/$0315`, saves the previous vector, programs the timer for the current BPM, and enables the CIA1 timer A interrupt.

**Interrupts are disabled for the duration of the IRQ installation.**

### `modplay_stop`

```c
void modplay_stop(void);
```

Stop playback. Silences all 4 MOD channels. Restores the previous `$0314/$0315` vector and restores CIA1 timer A to ~50 Hz (KERNAL compatible).

### `modplay_pause`

```c
void modplay_pause(void);
```

Suspend playback without losing position. The CIA timer continues running but the tick handler is a no-op. All 4 channels are silenced immediately.

### `modplay_resume`

```c
void modplay_resume(void);
```

Resume from the paused position.

### `modplay_set_master_volume`

```c
void modplay_set_master_volume(unsigned char vol);
```

Set the global volume scalar (0–63, default 63). Values above 63 are clamped. Applied every tick to all channels:

```
ua_vol = channel_vol × master_vol / 64
```

where `channel_vol` is 0–64 (ProTracker scale) and `ua_vol` is 0–63 (UA volume register is 6-bit; writing values > 63 silently ignores the upper bits, producing inconsistent loudness across samples).

### `modplay_set_stereo`

```c
void modplay_set_stereo(char enable);
```

`enable = 1`: ProTracker hard panning — channels 0, 3 hard-left; channels 1, 2 hard-right.
`enable = 0`: all channels centred (mono-compatible output).

Panning is applied on the next note trigger.

### Status accessors

```c
unsigned char modplay_get_order(void);    // current order index (0 … song_length-1)
unsigned char modplay_get_pattern(void);  // current pattern number
unsigned char modplay_get_row(void);      // current row within pattern (0–63)
unsigned char modplay_get_bpm(void);      // current BPM
unsigned char modplay_is_playing(void);   // 1 if playing and not paused
```

### Global player state

The full player state is accessible via the `modplay` global:

```c
extern mod_player_t modplay;
```

Key fields:

| Field | Type | Description |
|-------|------|-------------|
| `modplay.sample[n]` | `mod_sample_t` | Per-sample info (address, length, loop, volume, finetune) |
| `modplay.channel[n]` | `mod_channel_t` | Per-channel playback state |
| `modplay.song_length` | `unsigned char` | Order table length |
| `modplay.num_samples` | `unsigned char` | 15 or 31 |
| `modplay.num_patterns` | `unsigned char` | Number of unique patterns |
| `modplay.loop_song` | `unsigned char` | 1 = loop at end (default 1) |
| `modplay.stereo` | `unsigned char` | 1 = hard pan, 0 = mono |
| `modplay.master_volume` | `unsigned char` | 0–63 (default 63) |

---

## 7. MOD Player — Effects Reference

The player implements all standard ProTracker effects plus the `E` extended set.

### Normal effects (upper nibble of effect field)

| Code | Name | Implemented | Description |
|------|------|-------------|-------------|
| `0xy` | Arpeggio | Partial | Alternates between base note and +x / +y semitones per tick |
| `1xx` | Portamento up | ✓ | Decrease period by `xx` per tick (raise pitch) |
| `2xx` | Portamento down | ✓ | Increase period by `xx` per tick (lower pitch) |
| `3xx` | Portamento to note | ✓ | Slide period toward target note at speed `xx` |
| `4xy` | Vibrato | ✓ | Oscillate pitch; `x`=speed, `y`=depth |
| `5xy` | Porta + volume slide | ✓ | Effect 3 + volume slide simultaneously |
| `6xy` | Vibrato + volume slide | ✓ | Effect 4 + volume slide simultaneously |
| `7xy` | Tremolo | ✓ | Oscillate volume; `x`=speed, `y`=depth |
| `8xx` | Set panning | ✓ | Set UA pan register; `xx` hi-nibble × 17 → 0–255 |
| `9xx` | Sample offset | ✓ | Start sample playback `xx × 256` bytes into the sample |
| `Axy` | Volume slide | ✓ | +`x` or −`y` volume per tick |
| `Bxx` | Jump to order | ✓ | Jump to order table position `xx` |
| `Cxx` | Set volume | ✓ | Set channel volume to `xx` (0–64) |
| `Dxy` | Pattern break | ✓ | Break to row `x×10+y` in next pattern |
| `Exx` | Extended | ✓ | See table below |
| `Fxx` | Set speed | ✓ | `xx < $20` sets ticks-per-row; `xx ≥ $20` sets BPM |

### Extended effects (`E` — lower nibble of effect, upper nibble of param)

| Code | Name | Implemented | Description |
|------|------|-------------|-------------|
| `E0x` | Filter | — | Hardware filter; not applicable to UA |
| `E1x` | Fine porta up | ✓ | Decrease period by `x` once (tick 0 only) |
| `E2x` | Fine porta down | ✓ | Increase period by `x` once |
| `E3x` | Glissando | — | Quantise porta to nearest semitone (not implemented) |
| `E4x` | Set vibrato waveform | ✓ | 0=sine, 1=ramp, 2=square, 3=random |
| `E5x` | Set finetune | ✓ | Override finetune of current sample |
| `E6x` | Pattern loop | ✓ | Loop rows within current pattern |
| `E7x` | Set tremolo waveform | ✓ | 0=sine, 1=ramp, 2=square, 3=random |
| `E8x` | Panning (alternate) | — | Not supported |
| `E9x` | Note retrigger | ✓ | Retrigger note every `x` ticks |
| `EAx` | Fine volume up | ✓ | Volume + `x` once |
| `EBx` | Fine volume down | ✓ | Volume − `x` once |
| `ECx` | Cut note | ✓ | Set volume to 0 after `x` ticks |
| `EDx` | Note delay | ✓ | Delay note trigger by `x` ticks |
| `EEx` | Pattern delay | ✓ | Delay advancing to next row by `x` row-cycles |
| `EFx` | Invert loop | — | Not supported |

---

## 8. Typical Usage Patterns

### Minimal: detect, load, play

```c
#include "audio.h"
#include "modplay.h"
#include "ultimate_common_lib.h"
#include "ultimate_dos_lib.h"

void main(void) {
    // Detect hardware
    if (!uii_detect()) { /* no Ultimate */ return; }
    if (!audio_detect()) { /* no audio module */ return; }

    // Load MOD into REU at 64 KB offset
    uii_change_dir("/usb0/music/");
    if (!modplay_load("mysong.mod", 0x010000)) return;
    if (!modplay_init(0x010000)) return;

    // Set options and start
    modplay.loop_song = 1;
    modplay_set_master_volume(63);   // 0–63 (UA register is 6-bit)
    modplay_set_stereo(1);
    modplay_start();

    // Main loop — MOD plays in background
    while (modplay_is_playing()) {
        // demo code here
        // check modplay_get_row() / modplay_get_pattern() for sync
    }

    modplay_stop();
}
```

### Display sync: flash border on pattern change

```c
unsigned char last_pat = 0xFF;

while (1) {
    unsigned char pat = modplay_get_pattern();
    if (pat != last_pat) {
        *(volatile unsigned char *)0xD020 = pat & 0x0F;
        last_pat = pat;
    }
}
```

### Playing a sound effect on channel 4 (while MOD plays on 0–3)

```c
// Sample data assumed to be in REU at known address
void play_sfx(unsigned long reu_addr, unsigned long len, unsigned rate) {
    audio_channel_play(4, reu_addr, len, rate, 200, AUDIO_PAN_CENTRE);
}
```

Calculate rate for a given frequency (Hz):
```c
unsigned rate_for_hz(unsigned hz) {
    return (unsigned)(6250000UL / (unsigned long)hz);
}
// 8000 Hz → 781, 11025 Hz → 566, 22050 Hz → 283, 44100 Hz → 142
```

### Looping background ambience on channel 5

```c
void play_ambient(unsigned long reu_addr, unsigned long length,
                  unsigned long loop_a, unsigned long loop_b) {
    audio_channel_loop(5,
        reu_addr, length, loop_a, loop_b,
        781,            // ~8 kHz
        128,            // half volume
        AUDIO_PAN_CENTRE);
}
```

### Tempo change mid-song (via effect, not direct API)

The MOD player respects `Fxx` effects in the pattern data. To change BPM from code, write directly to `modplay.bpm`; the timer is reprogrammed at the next tick 0.

```c
modplay.bpm = 160;  // speed up; takes effect on next row
```

### Pause menu overlay

```c
void open_pause_menu(void) {
    modplay_pause();
    // draw menu, handle input
    modplay_resume();
}
```

---

## 9. Memory Layout and Constraints

### C64 RAM used by the library

| Area | Approximate size |
|------|----------------|
| `mod_player_t modplay` (global struct) | ~700 bytes |
| `mod_row_buf` (16-byte scratch) | 16 bytes |
| `mod_saved_irq` (2-byte IRQ save) | 2 bytes |
| Code (audio.c + modplay.c) | ~3–4 KB |
| **Total** | ~4–5 KB |

The `mod_player_t` struct includes the full `order_table[128]` and `sample[31]` arrays inline. If RAM is tight, reduce `MOD_MAX_SAMPLES` to 15 and patch the struct declaration.

### REU usage

The MOD file occupies REU starting at `reu_addr`. Typical 4-channel demo MODs are 200 KB–1 MB. The U64's 16 MB REU holds multiple MODs simultaneously at different offsets.

**Recommended REU layout:**

```
$000000–$00FFFF   64 KB   free (avoid: some demos use REU $0 as swap)
$010000–$?FFFFF   var     MOD file(s)
$?00000–$FFFFFF   var     Sample banks / other data
```

### Pattern row reads

Every new row triggers one REU DMA fetch of **16 bytes** (`reu_fetch`). At 50 Hz ticks and 6 ticks/row, this is `50 / 6 ≈ 8` DMA fetches per second — negligible overhead.

---

## 10. Timing and IRQ Details

### CIA1 timer A take-over

`modplay_start()` **replaces** the system CIA1 timer A IRQ. The KERNAL's 50 Hz clock tick (used for cursor blink, `STOP` polling, etc.) stops while the MOD player is running. This is normal for demos. The previous `$0314/$0315` vector is saved and restored by `modplay_stop()`.

The timer A interrupt mask (`$DC0D`) is set to enable only timer A. No other CIA1 interrupts fire.

The system's CIA1 timer A setting (~19 700 for 50 Hz PAL) is restored by `modplay_stop()`.

### IRQ handler (`modplay_irq_entry`)

The handler is written as a pure `__asm` function to avoid Oscar64 generating a C function prologue/epilogue:

```
C64 IRQ vector ($FFFE) → $EA31 (KERNAL IRQ)
KERNAL: push A, X, Y
KERNAL: JMP ($0314) → modplay_irq_entry
  Read $DC0D (acknowledges timer A interrupt)
  If timer A bit set: JSR modplay_tick
  JMP $EA7E  (KERNAL fake IRQ: restore A/X/Y, RTI)
```

`modplay_tick` is a regular C function. It may use A, X, Y freely (they are already saved by the KERNAL before our handler runs).

### Duration of `modplay_tick`

At 64 MHz turbo speed:
- REU DMA for 16 bytes: ~1–2 μs
- Per-channel effect processing: ~5–20 μs
- UA register writes: ~2–5 μs per channel
- **Estimated total per tick:** < 50 μs at 64 MHz

At 1 MHz (no turbo): the same code takes ~1–3 ms per tick. At 50 Hz, this consumes 5–15% of CPU time. Running the MOD player at 1 MHz is possible but leaves less time for demo effects. Turbo mode is strongly recommended.

### Synchronising demo effects to the beat

```c
// Raster-based sync: check row or order after the VIC raster hits a safe line
while (modplay_get_row() != 0) {}  // wait for row 0
// execute beat-sync effect
```

Or use the order/pattern change as a trigger (see §8 example).

---

## 11. Limitations and Known Issues

### Arpeggio (effect 0xy)

The arpeggio effect requires a note-to-rate lookup table to correctly compute +x and +y semitone offsets. The current implementation has a partial approximation that works for zero arpeggios (0-effect with data = 0) but may produce slightly inaccurate pitches for non-zero arpeggio values. If arpeggio accuracy is critical, provide a precomputed note rate table and replace the `FX_ARPEGGIO` case in `fx_tick`.

### Glissando (effect E3x)

Glissando mode (quantise portamento to nearest semitone) is noted as unimplemented. Portamento-to-note (`3xx`) works normally without glissando quantisation.

### Finetune precision

Finetune adjustment uses an integer linear approximation rather than the exact `2^(finetune/96)` formula. For finetune values of ±1–2 the error is negligible; for ±7–8 the error is about 0.3%. For exact finetune, replace `period_finetune()` with a 576-entry precomputed table.

### 15-sample format

15-sample (pre-1987) MODs are auto-detected and supported. The header parsing uses `600 + num_patterns * 1024` as the sample data base offset (no 4-byte magic marker in these files).

### CIA1 timer

`modplay_start()` takes over CIA1 timer A completely. KERNAL services that depend on CIA1 timer A (cursor blink, `TI` clock, `STOP` key check) stop working while the MOD player is active. Restore them with `modplay_stop()` when the demo ends.

`modplay_tick()` reprograms CIA1 timer A on every tick 0. This means BPM changes via `Fxx` (or direct `modplay.bpm` write) take effect on the very next row boundary.

### REU address range

Sample addresses written to the Ultimate Audio start register are 32-bit values mapped directly into the U64's SDRAM. The UA module and REU share the same SDRAM address space. A MOD loaded by `modplay_load()` must not exceed address `$00FFFFFF` (16 MB). If the entire MOD (header + patterns + samples) fits within 16 MB, no wrap-around issues occur.

### Channel 0 used for `audio_detect()`

`audio_detect()` uses channel 0 for its probe sequence and calls `audio_reset()` on success. Do not call `audio_detect()` while audio is playing — call it once during initialisation.
