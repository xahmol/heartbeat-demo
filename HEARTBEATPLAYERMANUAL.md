# Heartbeat Player Library Manual

**C/Oscar64 port of the Heartbeat Soundtracker standalone player, for the Ultimate 64**

Library files:
- `include/hbplayer.h` / `include/hbplayer.c`

Depends on (already part of this project, see their own manuals):
- `include/audio.h` / `include/audio.c` — Ultimate Audio DMA layer ([`ULTIMATEAUDIOMANUAL.md`](ULTIMATEAUDIOMANUAL.md))
- `include/ultimate_common_lib.h/.c`, `include/ultimate_dos_lib.h/.c` — UCI file/REU access ([`UCILIBMANUAL.md`](UCILIBMANUAL.md))

Ported from the licensed Heartbeat Soundtracker standalone player (6502 assembly)
by Aleksi Eeben / Eight Bit Shed, used and redistributed with the author's explicit
permission — see [`NOTICE.md`](NOTICE.md). The original assembly source is **not**
part of this repository (local-only reference, gitignored); only this C conversion
is public.

For internal design (IRQ architecture, data flow, zero-page handling), see
[`ARCHITECTURE.md`](ARCHITECTURE.md). This manual covers the public API, the song
file format, and the track-command reference.

---

## Contents

1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Public API Reference](#3-public-api-reference)
4. [Song Data Structures](#4-song-data-structures)
5. [Player-Internal State](#5-player-internal-state)
6. [Track Command Reference](#6-track-command-reference)
7. [Firmware Prerequisites](#7-firmware-prerequisites)
8. [Fidelity Notes and Known Limitations](#8-fidelity-notes-and-known-limitations)

---

## 1. Overview

Heartbeat Soundtracker is a music tracker built specifically around the Ultimate 64's
combination of SID chip(s) + Ultimate Audio DMA sample channels + 16 MB REU. A song
(`.reu` file) contains sequencer/pattern data, up to 8 SID chips' worth of 3-channel
synth tracks, and 7 Ultimate Audio DMA sample tracks, all driven by one tick IRQ.

This library plays such a song file on real Ultimate 64 hardware:

- Loads a `.reu` file into REU via UCI (auto-scanning SD/USB drives).
- Streams pattern data from REU one row at a time as playback advances (not
  preloaded into C64 RAM — patterns can be arbitrarily large).
- Drives up to 8 SID chips (3 channels each) and all 7 Ultimate Audio DMA channels
  from a single CIA1 Timer A interrupt, tempo-synced via the song's own BPM.
- Implements the full per-tick modulation set: vibrato, pulse-width sweep, SID
  filter cutoff sweep, wave/arpeggio table stepping, and portamento (both SID and
  Ultimate Audio).
- Implements all 11 in-pattern track commands (`Pa`/`Bt`/`Dn`/`Fi`/`Iv`/`Le`/`Co`/
  `Po`/`Up`/`Vo`/`Xt`).
- Supports manual one-off sample triggering (`hb_play_fx`/`hb_stop_fx`) independent
  of song playback, e.g. for a demo's own sound effects.
- Detects PAL/NTSC at startup and adjusts SID frequency tables and tempo timing
  accordingly.

The goal of this port is **bit-exact behavioral parity** with the original assembly
player, not a reinterpretation — every routine is a direct, traceable translation,
documented with its own comment pointing at the corresponding label in `player.s`.

---

## 2. Quick Start

```c
#include "hbplayer.h"
#include "turbo.h"

// Turbo MUST be engaged before starting playback -- hb_tick's per-tick work
// (full Modulations pass over up to 24 SID channels + 7 UA channels, every
// tick) does not reliably fit the ~5000 CPU cycles the CIA1 Timer A period
// gives at 1 MHz. See ARCHITECTURE.md's "Timing" section.
turbo_fast();

if (hb_load("My Song.reu", 0x000000UL))
{
    hb_detect_ntsc();      // must run before hb_init()
    hb_init(0, 1);         // seq_start_pos=0, play_mode=1 (play song)

    // Playback now runs entirely in the background via the tick IRQ.
    // Your own code is free to run here -- poll hb_ext_out for Xt-command
    // sync cues, trigger one-off effects with hb_play_fx(), etc.

    for (;;)
    {
        // ... your demo code ...
    }
}
```

To stop playback and silence everything:

```c
hb_stop_all();
```

---

## 3. Public API Reference

### `char hb_load(char *filename, unsigned long reu_addr)`

Scans SD/USB drives for `filename` (via `uii_scan_media`/`uii_find_media_path`,
falling back to the U64's configured home directory first), loads it into REU
starting at `reu_addr` via chunked UCI transfer, then reads the `$2000`-byte
song-data header (fixed REU offset `reu_addr + $00E000`, matching the file's own
internal layout) into `hb_songdata`. Sets `hb_state.reu_song_base`.

Returns 1 on success, 0 if the file could not be found or loaded.

Install-path convention: the demo scans for the file under
`idi8b/<your-project-folder>/` — see `hb_load()`'s own comment in `hbplayer.c` and
keep this in sync with your Makefile's `INSTALL_PATH`.

### `char hb_detect_ntsc(void)`

Raster-line PAL/NTSC detection. Sets `hb_state.ntsc_detected` (1 = NTSC). **Must be
called before `hb_init()`** — it selects which embedded SID frequency table
(`hb_palfreq`/`hb_ntscfreq`) and BPM timer delta get used from then on.

### `void hb_init(unsigned char seq_start_pos, unsigned char play_mode)`

Full player (re-)initialization: resets all SID/UA working state, sets the song's
starting tempo, installs the tick IRQ at `$0314`/`$0315`, enables CIA1 Timer A and
a raster IRQ (for keyboard scanning independent of tempo), and starts playback from
sequencer step `seq_start_pos`.

`play_mode`: `0` = idle (registers still flush every tick, e.g. for envelope decay,
but no new rows are played), `1` = play the song. Can be called again at any time
to restart the song from a given step (see the test harness's `SPACE` binding).

### `void hb_stop_all(void)`

Stops playback and resets SID working state (`hb_state.play_mode = 0` +
re-runs the SID-side of init). Matches the original exactly: this does **not**
reset Ultimate Audio channel state (the reference player's own behavior — ported
as-is, not "fixed").

### `void hb_set_tempo(unsigned char bpm_minus_64)`

Reprograms CIA1 Timer A from the embedded BPM table. `bpm_minus_64` = BPM − 64
(range 0–255 → 64–319 BPM). Applies the NTSC timer delta automatically if
`hb_state.ntsc_detected`. Safe to call from within the tick IRQ (used internally by
the `Bt` track command) — preserves the caller's interrupt-enable state rather than
unconditionally re-enabling interrupts.

### `void hb_play_fx(unsigned char ch, unsigned char sample, unsigned char note)`

Manually trigger `sample` (1–64, indexes `hb_songdata.sample_params`) at `note`
(2–95; `0x26` = C-4 = 44100 Hz) on Ultimate Audio channel `ch` (0–6), independent of
song playback. Applies the sample's own note-pitch and transpose settings, exactly
as if it had been triggered from a pattern row. Safe to call from your main loop at
any time; internally disables/re-enables interrupts around the trigger.

### `void hb_stop_fx(unsigned char ch)`

Stops the note on Ultimate Audio channel `ch` (0–6). If the channel is in
loop-with-release mode, releases the loop first rather than cutting immediately.

### `void hb_fetch_pattern_row(void)`

Fetches the next pattern row into `hb_row_buf` via REU streaming, advancing the
sequencer/pattern pointers as needed. Called internally by the tick dispatch; only
useful to call directly for diagnostics (e.g. printing raw row bytes).

### `void hb_vis_reset(void)`

Clears the visualizer event queue (`hb_vis_events[]`) and resets
`hb_vis_event_count` to 0. Not called internally by the player — call it from your
own code when switching visualizer modes or restarting the song, if you want a
clean slate rather than waiting for the next tick's natural reset.

### Visualizer hooks

Port of the original's `visualizerout` block — every SID and Ultimate Audio note
trigger populates a rolling queue of `(note, sound, channel, velocity)` events,
reset at the start of every tick (`hb_state.play_mode` doesn't matter — the queue
resets even when playback is fully off).

```c
#define HB_VIS_MAX_EVENTS 32

typedef struct {
    unsigned char note;
    unsigned char sound;
    unsigned char velocity;  // 0-63 perceptual loudness estimate, not a raw register
    unsigned char channel;   // 0-6 = UA channels, 7-30 = SID channels
} hb_vis_event_t;

extern hb_vis_event_t hb_vis_events[HB_VIS_MAX_EVENTS];
extern unsigned char  hb_vis_event_count; // valid entries this tick
```

**Read this once per your own update cycle** (e.g. once per VIC frame), not once
per tick — ticks fire far faster (~195 Hz) than any practical redraw rate, so
`hb_vis_events[0 .. hb_vis_event_count-1]` is a **per-tick snapshot that gets
overwritten on the next tick**, not a history buffer. A visualizer wanting smooth
VU-meter-style decay needs to accumulate/decay values itself across reads.

SID channel numbers: `7 + sid_idx*3 + ch_idx` (so chip 0's 3 channels are 7, 8, 9;
chip 1's are 10, 11, 12; and so on). UA channels use their own index (0-6)
directly. `velocity` comes from the channel's ADSR envelope nibbles via a
perceptual-loudness heuristic (attack/decay/sustain/release weighted and clamped
to 0-63) — see `hb_vis_adsr_weight()` in `hbplayer.c` if you need the exact
formula.

`hb_vis_sound` for Ultimate Audio channels is **not** the sample number directly —
the original reverses it (`sound_out = ~(sound_in - 1) & 0x3F, then +1`), a
display/palette convention with no further documented rationale, ported literally.
SID channels store the sample/instrument number as-is.

### Globals

| Symbol | Purpose |
|---|---|
| `hb_songdata` | The loaded song's `$2000`-byte header (patterns/sequencer tables, sample/instrument parameter tables) — see [§4](#4-song-data-structures). |
| `hb_state` | Player transport state (tempo, current row/pattern/sequencer position, mute tracking) — see [§5](#5-player-internal-state). |
| `hb_row_buf[64]` | The current pattern row, as streamed from REU. |
| `hb_sids[HB_MAX_SIDS]` | Per-SID-chip working state (filter, 3× per-channel state) — see [§5](#5-player-internal-state). |
| `hb_ua[HB_UA_CHANNELS]` | Per-Ultimate-Audio-channel working state — see [§5](#5-player-internal-state). |
| `hb_ext_out` | Sync-output byte written by the `Xt` command (nonzero param) — poll this from your own code to react to music-synced cues. Never cleared automatically. |
| `hb_vis_events[HB_VIS_MAX_EVENTS]` / `hb_vis_event_count` | The visualizer event queue — see above. |

`HB_MAX_SIDS` = 8, `HB_UA_CHANNELS` = 7 (fixed by Ultimate Audio hardware),
`HB_VIS_MAX_EVENTS` = 32.

---

## 4. Song Data Structures

These layouts are **dictated by the `.reu` file format** — byte offsets are exact
and must not be reordered. Traced from the original player's `SONGDATA` label
block and the `PlaySampleNote`/`ModulateChannel` record-field reads.

### Top-level song data (`hb_songdata_t`, exactly `$2000` bytes)

| Offset | Field | Size | Notes |
|---|---|---|---|
| `$0000` | `sequencer_patterns[256]` | 256 | Pattern # per sequencer step (`$00`=end, `$FF`=loop, `$01`-`$40`=pattern) |
| `$0100` | `sequencer_transpose[256]` | 256 | Also the loop-target step # when `patterns[step]==$FF` |
| `$0200` | `sequencer_ultmutes[256]` | 256 | Bitmask, 7 UA channels, bit set = **not** muted |
| `$0300` | `sequencer_sidmutes[256]` | 256 | Bitmask, up to 8 SID chips, bit set = **not** muted |
| `$04E8` | `sid_volumes[8]` | 8 | Initial per-SID master volume |
| `$0540` | `starting_tempo` | 1 | BPM − 64 |
| `$0541` | `hardrestart_time` | 1 | Tick countdown value at which SID channels get a clean hard-restart ahead of the row's real note-on |
| `$0547` | `song_pattern_length` | 1 | Default pattern length (≤ 64) |
| `$0548` | `hardrestart_sr` | 1 | Envelope S/R applied during hard-restart |
| `$0549` | `hardrestart_ad` | 1 | Envelope A/D applied during hard-restart |
| `$054A` | `hardrestart_gateon_time` | 1 | Tick countdown value at which the early-gate-on pre-arm runs |
| `$054B` | `hardrestart_gateon_wave` | 1 | Waveform byte written during early-gate-on |
| `$0550` | `sid_addresses[16]` | 16 | 8 × (lo, hi) SID chip base addresses; `$0000` = chip slot unused |
| `$0800` | `sample_params[64]` | 2048 | 32 bytes/record — see below |
| `$1000` | `inst_params[64]` | 4096 | 64 bytes/record — see below |

Unmapped ranges (`$0400`–`$04E7`, `$04F0`–`$053F`, `$0560`–`$07FF`) are never read
by the player — editor-only metadata.

### `hb_sample_params_t` — one Ultimate Audio sample record (32 bytes)

| Offset | Field |
|---|---|
| `$00`-`$0F` | (unused by the player — editor metadata) |
| `$10` | `volume` |
| `$11` | `pan` |
| `$12` | `note_pitch` — semitones; transpose applies unless the drum flag is set |
| `$13` | `finetune` (signed) |
| `$14` | `portamento` speed (`$00` = instant) |
| `$15` | `reu_bank` — sample REU address = `0x01000000 \| (reu_bank << 16)`; every sample begins at a 64 KB-aligned REU offset |
| `$16`-`$18` | `length[3]` — 24-bit, LSB-first |
| `$19`-`$1B` | `loop_a[3]` — 24-bit, LSB-first |
| `$1C`-`$1E` | `loop_b[3]` — 24-bit, LSB-first |
| `$1F` | `flags` — bit 7 = drum flag (no transpose); bits 0-1 = loop mode (`0`=none, `3`=one-shot cropped to the loop A/B region, else = loop) |

### `hb_inst_params_t` — one SID instrument record (64 bytes)

| Offset | Field |
|---|---|
| `$00`-`$0F` | Instrument name (editor-only text, never read by the player) |
| `$10` | `env_ad` |
| `$11` | `env_sr` |
| `$12` | `finetune` (signed) |
| `$13` | `portamento` speed |
| `$14` | `pwm_start` — `$00` = don't reset PW (keep current sweep direction/value) |
| `$15` | `pwm_rate` |
| `$16` | `pwm_topbottom` — top/bottom nibbles |
| `$17` | `vib_delay` |
| `$18` | `vib_width` |
| `$19` | `vib_rate` |
| `$1A` | `filter_type` — `0` = no filter for this channel; bit 4 = cutoff-mod bounce-vs-stop flag |
| `$1B` | `filter_resonance` |
| `$1C` | `cutoff_init` — `0` = don't reset cutoff (keep current direction) |
| `$1D` | `cutoff_mod` — signed rate/direction |
| `$1E` | `cutoff_top` |
| `$1F` | `cutoff_bottom` |
| `$20`-`$2F` | `wave_table[16]` — waveform step table |
| `$30`-`$3F` | `arp_table[16]` — arpeggio step table (shares step indices with `wave_table`; `$FC`/`$FD`/`$FE` in the wave table are envelope-AD/envelope-SR/speed commands whose parameter comes from the matching arp-table slot; `$FF` is a loop-to-step command) |

**Note:** an earlier revision of this port had `wave_table`/`arp_table` at
`$00`-`$0F` — that was wrong (confirmed via hardware cross-check against real
song data) and has been corrected to the offsets above.

---

## 5. Player-Internal State

Unlike §4, these structures are **freely designed** — not part of the file format,
re-initialized fresh at `hb_init()`/`hb_stop_all()`.

### `hb_state_t`

| Field | Meaning |
|---|---|
| `play_mode` | `0`=idle, `1`=play song, bit 7 set = fully off (skips even register flush) |
| `tempo` | BPM − 64 |
| `tempo_ticks` | Ticks per row, from the BPM table |
| `tick` | Countdown to the next row |
| `patt_ptr` / `patt_bank` | Current pattern's REU offset/bank |
| `patt_length` / `patt_step` | Current pattern length and row index |
| `seq_start_pos` / `seq_step` | Sequencer position |
| `last_ua_mutes` / `last_sid_mutes` | Previous row's mute bitmasks, for mute-transition detection |
| `transpose_now` | Current sequencer step's transpose value |
| `ntsc_detected` | Set by `hb_detect_ntsc()` |
| `reu_song_base` | REU address the song header was loaded to |

### `hb_sid_chip_t` (× `HB_MAX_SIDS`) and `hb_sid_channel_t` (× 3 per chip)

Chip-level: filter cutoff (working value + sweep bounds/rate/bounce-flag),
`filt_ctrl` (per-channel filter-enable bits + resonance), `volume` (master vol +
filter-type nibble), `addr` (real SID chip address; `0` = unpopulated slot).

Per-channel: base frequency + portamento target/speed, vibrato (delay/phase/
width/rate/frac), PWM (rate + top/bottom bounds), finetune, instrument
pointer/wave-arp-table-stepping state, and the **active register image**
(`sid_freq`, `sid_pw`, `sid_wave`, `sid_env_ad/sr`) that gets flushed to real SID
hardware once per tick.

### `hb_ua_channel_t` (× `HB_UA_CHANNELS`)

Frequency + portamento target/speed, `shadow_gate` (the deferred control byte —
see [`ARCHITECTURE.md`](ARCHITECTURE.md#shadow-flush-pattern) for why only this one
field is deferred), finetune, note pitch/drum-flag/loop-mode, and the active
sample index.

---

## 6. Track Command Reference

A note byte with bit 7 set (in `hb_row_buf`) is a track command, not a note. Its
low 7 bits select the command (`$00`-`$0A`); its parameter is the channel's own
"+1" byte (the same slot that normally holds the sample/instrument number). Cmd
`$0B`-`$7F` are undefined and ignored.

| # | Name | UA channel | SID channel | Cmd channel |
|---|------|:---:|:---:|:---:|
| `$00` | **Pa** — pan | set pan (0-15, immediate) | ignored | ignored |
| `$01` | **Bt** — tempo | `hb_set_tempo(param)` | same | same |
| `$02` | **Dn** — slide down | portamento speed=`param`, target=`$0000` | same | slides **all** SID+UA channels toward `$0000` |
| `$03` | **Fi** — finetune | set finetune (signed) | same | ignored |
| `$04` | **Iv** — SID volume | applies to **all** SID chips (low nibble) | applies to **current chip only** | applies to all chips |
| `$05` | **Le** — pattern length | set (clamped to 64) | same | same |
| `$06` | **Co** — filter cutoff | ignored | set chip filter cutoff (`param<<5`, `\|0x8000`) | ignored |
| `$07` | **Po** — portamento speed | set speed only, no target change | same | ignored |
| `$08` | **Up** — slide up | portamento speed=`param`, target=`$17C0` | same | slides **all** SID+UA channels toward `$17C0` |
| `$09` | **Vo** — volume | set volume (0-63, immediate) | set envelope sustain nibble (`param<<4`) | ignored |
| `$0A` | **Xt** — sync/kill | nonzero param → `hb_ext_out=param`; `0` → kill channel | same, or (Cmd channel) full re-init | nonzero → sync out; `0` → full re-init (`hb_init_ua_and_sids()`) |

---

## 7. Firmware Prerequisites

Same hardware requirements as the rest of this project — see the main
[`README.md`](README.md#requirements) — plus the SID-specific settings from the
song's own `Heartbeat.cfg` (SID chip addressing, filter curve, audio mixer levels).
If a song plays but some SID channels are silent, check your U64's **Audio Mixer**
firmware settings (`F2` → Audio Mixer) — `Vol UltiSid 1`/`Vol UltiSid 2` must not be
`OFF`, and if physical SID sockets are populated and enabled, `$D400`/`$D420`
writes may route there instead of the internal UltiSID emulation (see
`SID Sockets Configuration` / `SID Addressing` in the firmware menu).

---

## 8. Fidelity Notes and Known Limitations

- **Bit-exact by design, not accident.** Several routines look unusual in C
  (asymmetric bit-mask tables, direction-preserving sign logic, a fixed-point
  "$8000-based" internal representation for filter cutoff and pulse width) because
  they're literal translations of specific 6502 self-modifying-code or
  carry-propagation tricks in the original — see the comment above each such
  function in `hbplayer.c` for the exact source line reference.
- **`hb_stop_all()` does not reset Ultimate Audio state** — matches the original's
  own `StopAllSound`, which only re-runs the SID-side init.
- **`MusicPlayMode == 2`** (pattern-loop/editor live-preview mode) is not
  implemented — the standalone player itself never uses it either.
- **Visualizer output hooks** from the original (conditional `visualizerout`
  blocks) are not ported — this library has no visualizer.
- PWM/filter/cutoff internal representations use the same "$8000 marker bit,
  hardware ignores the unused high bits" trick as the original — see
  [`ARCHITECTURE.md`](ARCHITECTURE.md) if you need to read/modify these fields
  directly instead of through the public API.
