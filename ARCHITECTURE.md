# heartbeat-demo — Architecture Reference

This document describes the internal design of the Heartbeat Soundtracker C/Oscar64
port (`include/hbplayer.h/.c`): the tick/IRQ architecture, data flow through a
single tick, the shadow-flush pattern, and the zero-page verification methodology
used throughout the port. It is intended as a reference for contributors and for AI
coding assistants working on the codebase.

For the public API and song file format, see
[`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md). For general Oscar64
compiler gotchas (not specific to this project), see
[`oscar64manual.md`](oscar64manual.md).

---

## Table of Contents

- [1. Porting Philosophy](#1-porting-philosophy)
- [2. Memory Layout](#2-memory-layout)
- [3. IRQ / Tick Architecture](#3-irq--tick-architecture)
  - [Dual dispatch: raster vs. CIA1 Timer A](#dual-dispatch-raster-vs-cia1-timer-a)
  - [The `__interrupt` auto-save gap](#the-__interrupt-auto-save-gap)
  - [Zero-page gap analysis methodology](#zero-page-gap-analysis-methodology)
- [4. One Tick, Start to Finish](#4-one-tick-start-to-finish)
- [5. Shadow-Flush Pattern](#5-shadow-flush-pattern)
- [6. Frequency Conversion](#6-frequency-conversion)
- [7. Fixed-Point Internal Representations](#7-fixed-point-internal-representations)
- [8. Verification Methodology](#8-verification-methodology)
- [9. Build System Overview](#9-build-system-overview)

---

## 1. Porting Philosophy

This port targets **bit-exact behavioral parity** with the licensed 6502 assembly
reference (`reference/heartbeat-player-src/player.s`, local-only, gitignored — see
[`NOTICE.md`](NOTICE.md)), not a reinterpretation of what the player "should" do.
Where the original uses a carry-propagation trick, an asymmetric bit-mask table, or
a direction-preserving sign check, the C port reproduces the same arithmetic
exactly — even when a "cleaner" equivalent exists — because subtle behavioral
differences (an off-by-one in a wave/arp table start step, an uninitialized pulse
width) are audible bugs, and this project has hit several of exactly that shape.
See [§8](#8-verification-methodology) for how those were caught.

Every non-trivial ported function carries a comment naming the original label it
corresponds to (e.g. "port of `PlaySIDNote`") so the two sources can be diffed by
eye.

---

## 2. Memory Layout

Build region: `#pragma region(main, 0x0A00, 0xC000, ...)` — `$0A00`–`$BFFF`, with
`$01=$36` (`MMAP_NO_BASIC`: KERNAL + I/O visible, `$A000`–`$BFFF` always CPU RAM,
no BASIC ROM shadow). Code, data, BSS, heap, and stack all live in this one region;
exact sizes shift per build (check `build/*.map` for the current binary).

Key runtime data structures (all BSS, sized as documented in
[`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md#4-song-data-structures)):

| Symbol | Size | Contents |
|---|---|---|
| `hb_songdata` | `$2000` (8192 B) | The loaded song's header, copied from REU |
| `hb_state` | 19 B | Transport state |
| `hb_row_buf` | 64 B | Current pattern row |
| `hb_ua[7]` | 105 B (15 B/channel) | Ultimate Audio channel working state |
| `hb_sids[8]` | 840 B (105 B/chip: 12 B chip header + 3×31 B channels) | SID chip working state |

Embedded lookup tables (`#embed`, compiled in as `const` data — see
`hbplayer.c`'s top section):

| Table | Size | Source |
|---|---|---|
| `hb_bpmtable` | 768 B | `bpmtable.bin` — 3×256 pages: tempo_ticks, timer_lo, timer_hi |
| `hb_bpm_ntsc_add` | 256 B | `bpmtable-ntsc.bin` — additive NTSC timer delta |
| `hb_ultfreq` | 1536 B | `ult-freq.bin` — Ultimate Audio frequency table (clock-independent) |
| `hb_palfreq` | 1536 B | `pal-freq.bin` — SID frequency table, PAL |
| `hb_ntscfreq` | 1536 B | `ntsc-freq.bin` — SID frequency table, NTSC |

These `.bin` files live under `reference/heartbeat-player-src/bin/` — local-only,
required to build this project but never committed (see `hbplayer.c`'s top comment
for the exact `#embed` paths). **A checkout without the licensed reference source
cannot build `hbplayer.c`** — this is an accepted tradeoff for keeping the licensed
binary tables out of the public repo (see [`NOTICE.md`](NOTICE.md)).

REU layout is dictated by the song file format itself: the `$2000`-byte header at
REU offset `reu_song_base + $00E000`; each pattern occupies exactly 4096 bytes at
REU `(pattern_number + 15) << 12`; sample data at
`0x01000000 | (reu_bank << 16)`.

---

## 3. IRQ / Tick Architecture

### Dual dispatch: raster vs. CIA1 Timer A

`hb_init()` installs `hb_irq` at `$0314`/`$0315` and enables **both**:

- A fixed-line **raster IRQ** (line 0), independent of tempo — its only job is
  guaranteed ~50/60 Hz keyboard scanning, so the keyboard stays responsive
  regardless of how slow the current BPM's tick rate is.
- **CIA1 Timer A**, reprogrammed by `hb_set_tempo()` from the embedded BPM table —
  this is what actually drives `hb_tick()`.

`hb_irq` (a raw, prologue-free `__asm` block — not a C function; installed via
`*((void**)0x0314) = hb_irq`, never called with `hb_irq()`) checks `$D019` bit 0
first to tell the two apart:

```
hb_irq:
    lda $d019
    and #$01
    bne hb_irq_raster        ; VIC raster IRQ -> just ack + chain

    lda $02                  ; save/restore ZP $02 around the call -- see below
    pha
    jsr hb_tick
    pla
    sta $02
    lda $dc0d                ; ack CIA1 Timer A IRQ (clear-on-read)
    jmp $ea31

hb_irq_raster:
    sta $d019                ; ack raster IRQ
    jmp $ea31                ; $EA31's own sequence includes SCNKEY
```

Both branches chain to KERNAL `$EA31` (not `$EA81`, despite the reference using
`$EA81` — see the "PORT DEVIATION" comment on `hb_irq` in `hbplayer.c`: chaining to
`$EA81` under `MMAP_NO_BASIC` was hardware-confirmed to stop `hb_tick` firing after
~2 invocations, likely hitting an unprotected KERNAL/BASIC-ROM path that this
project's `$0310`/`$A002` startup patches don't cover for that entry point).
`$EA31`'s own sequence already includes `SCNKEY`, so the raster branch needs no
separate manual keyboard-scan call.

### The `__interrupt` auto-save gap

`hb_tick` is declared `__interrupt`, so Oscar64 auto-generates a save/restore
prologue for whatever zero-page locations it can **statically** determine the
function's call tree touches (as of the full Phase 9 call tree: `WORK+0..3`,
`P0`-`P10`, `ACCU+0..3`, `T0`-`T3`, and `$4D`-`$51`).

This analysis has a real, confirmed gap: **`mul16by8`** (Oscar64's runtime
multiply helper, pulled in by several `note << 6`-style computations across the
call tree) uses zero-page **`$02`** as scratch, and `$02` is never part of the
auto-saved set. Left unprotected, this silently corrupts `$02` for whatever else
the KERNAL/other code (running outside the tick IRQ) is using it for. `hb_irq`
protects it manually with a `pha`/`pla` pair around the `jsr hb_tick`, `$02` chosen
specifically because it's what the sweep in the next section found.

### Zero-page gap analysis methodology

This isn't a one-time fix — the call tree changed with every phase, and a new
runtime-helper dependency could introduce a new gap at any point. The method,
reapplied after every phase that touched `hb_tick`'s call tree:

1. Build with `-g` (`oscar64 ... -g -o=build/hbdemo-dbg.prg src/main.c`), producing
   `build/hbdemo-dbg.asm` with full source-line-annotated disassembly.
2. Locate `hb_tick`'s auto-save prologue in the `.asm` output — a run of
   `LDA <addr> / PHA` pairs at the top of the function — to get the exact
   currently-auto-saved set.
3. Find `hb_tick`'s real address range (next top-level label after it) and collect
   every `JSR` target inside it.
4. **Recursively repeat for every JSR target's own address range** (using the full
   label list, not just `hb_`-prefixed ones — runtime library routines like
   `mul16by8`/`divmod`/`divmod32` sit in the same address space and must be
   included). Watch for wrong range boundaries: picking the *next* label instead of
   the function's *actual* end pulls in unrelated code that happens to be laid out
   next in the binary and produces false-positive "JSR"s that don't belong to the
   call tree at all (this happened once in this project's own history — see the
   commit history around the Phase 9 re-verification).
5. For each function in the resulting reachable set, grep its disassembly for raw
   zero-page addressing (`LDA $XX` / `STA $XX` with a **bare 2-hex-digit operand,
   no `WORK`/`ACCU`/`P`/`T`-prefixed symbol name and no `#` immediate marker**) —
   that's the signature of an *unnamed* zero-page location Oscar64's own register
   allocator isn't tracking as part of the auto-save set.
6. Cross-reference every such location against the current auto-save set. Anything
   not covered needs manual protection in `hb_irq`, the same way `$02` is.

This sweep has been re-run after every phase that grew `hb_tick`'s call tree
(Phases 6 through 9) and found exactly one gap throughout the whole port ($02,
`mul16by8`) — `divmod`/`divmod32` (used by the octave-fold frequency lookups, see
[§6](#6-frequency-conversion)) only ever touch `WORK`/`ACCU`, which are already
covered.

---

## 4. One Tick, Start to Finish

`hb_tick()` (the `__interrupt` function called from `hb_irq`) mirrors the
original's `PlayerUpdate` dispatch exactly:

```
hb_tick:
    if play_mode has bit 7 set:
        return                              # skip everything, even register flush

    if play_mode != 0:                      # actively playing
        tick--
        if tick == 0:
            hb_play_pattern_row()           # → hb_cmd_channel_cmd (Cmd channel first)
                                             #   → hb_play_pattern_row_sid()
                                             #   → hb_play_pattern_row_ua()
                                             #   → update last_*_mutes, reset tick
        elif tick == hardrestart_time:
            hb_fetch_pattern_row()          # pre-fetch next row's notes
            hb_sid_hard_restart()           # clean-silence channels about to retrigger
        elif tick == hardrestart_gateon_time:
            hb_early_gate_on()              # pre-arm envelope/gate, avoids a click

    hb_modulations()                        # ALWAYS runs (even when idle/play_mode==0)
        → hb_modulate_sid_filter() × 8 chips
        → hb_modulate_channel() × 24 SID channels
        → hb_modulate_ua_channel() × 7 UA channels

    hb_register_update()                    # ALWAYS runs
        → hb_register_update_sid()          # flush SIDImage → real SID hardware
        → hb_register_update_ua()           # flush shadow_gate → UAControl
```

The three `tick ==` branches are mutually exclusive per tick (an `if`/`else if`
chain matching the original's `dec Tick / bne .1` / `cmp HARDRESTARTTIME` /
`cmp HARDRESTART_GATEON_TIME` structure) — at most one of row-play,
hard-restart-fetch, or early-gate-on happens on any given tick, but Modulations and
the register flush always run afterward regardless of which branch (or none) fired.

**Note trigger vs. modulation are separate concerns**, and this separation is the
single most important structural fact about this codebase: `hb_play_sid_note()`
(called from `hb_play_pattern_row_sid()`, itself called only when `tick==0`) sets
up a note's *initial* parameters — envelope, PWM start value, vibrato delay/width/
rate, filter type/resonance/cutoff-init, wave/arp table reset (`wave_arp_step`
starts at `0xFF` so the first `hb_modulate_channel()` pass reads real step `0`, not
step `1`). It does **not** compute the note's actual frequency or waveform — that
happens in `hb_modulate_channel()`, which runs **every tick**, including the same
tick as the trigger (since `hb_modulations()` runs after `hb_play_pattern_row()`
within one `hb_tick()` call).

This split exists in the original too, and porting it required going back and
adding note-trigger init that had been deliberately deferred to "the modulation
phase" in an earlier pass of this port. The lesson generalizes: **when adding a
modulation-sweep routine, always check whether the corresponding note-trigger init
was actually written**, because "the sweep looks right" and "the note plays" are
different claims, and a stuck-at-zero PWM register (0% duty cycle = silence) looks
identical to an idle channel in a raw memory dump.

---

## 5. Shadow-Flush Pattern

SID and Ultimate Audio use **different** deferred-write strategies, both ported
exactly from the original:

**SID**: every channel's full active-register image (`sid_freq`, `sid_pw`,
`sid_wave`, `sid_env_ad`/`sr`) is computed into `hb_sid_channel_t` fields
throughout the tick (by `hb_play_sid_note()` at trigger time, then continuously
recomputed by `hb_modulate_channel()`), and only actually **written to hardware**
once, at the very end of the tick, by `hb_write_one_sid()`
(`hb_register_update_sid()`'s per-chip helper). This batches all SID register
writes together for tight sync — every channel's frequency/waveform/envelope
changes land in the same "frame."

**Ultimate Audio**: only the gate/control byte (`shadow_gate`: `0x00`/`0x10`/
`0x11`/`0x13`) is deferred. Sample start address, length, loop points, volume,
pan, and playback rate are all written **directly to hardware immediately** at
trigger time (`hb_trigger_sample()`) or every tick (`hb_modulate_ua_channel()`'s
rate recompute) — there is no shadow/batch mechanism for these in the original,
so none was added here. Only the final "start/stop/loop" trigger byte needs to be
deferred, and `hb_register_update_ua()` flushes all 7 channels' `shadow_gate` to
`UAControl` at the same point in the tick as the SID flush.

---

## 6. Frequency Conversion

`hb_get_sid_freq()`/`hb_get_ultimate_freq()` convert a 16-bit "linear" frequency
(note × 64, an internal fixed-point unit used throughout — see
[§7](#7-fixed-point-internal-representations)) into the real hardware register
value, via **octave-folding + a per-octave lookup table**:

1. Fold the input down by subtracting 3 from the high byte repeatedly until it's
   below `$18` (24) — this maps any octave onto one of 8 "octave slots" within the
   embedded frequency table.
2. `shift = 7 - (octave_slot / 3)` for SID, `shift = octave_slot / 3` for Ultimate
   Audio (**reversed** — verified against source, not a typo) — the number of bits
   to right-shift the looked-up 16-bit table value by.
3. `page = (octave_slot % 3) * 256` selects one of 3 sub-tables within the octave
   slot's 768-byte block.
4. Look up the 16-bit value at `table[page + x]` (low byte) /
   `table[page + 0x300 + x]` (high byte, tables offset +3 pages from the low-byte
   tables), combine, and shift right by `shift`.
5. Ultimate Audio's version applies one more step: subtract 1 from the final
   result (a plain unsigned 16-bit `-= 1` already wraps/borrows correctly, no
   separate borrow-flag logic needed in C).

The original implements this via **self-modifying code** — patching a `JMP`
target's low byte to select the shift amount, and an `LDA` operand's high byte to
select the table page. The C port re-expresses this as ordinary indexed array
access + a runtime shift, verified mathematically equivalent by working through
both derivations and confirming the same bit positions result (both use the
`divmod` runtime helper for the `/3`/`%3`, already covered by the zero-page
auto-save set — see [§3](#3-irq--tick-architecture)).

---

## 7. Fixed-Point Internal Representations

Two working-state fields use a shared "`$8000`-based" 16-bit representation,
carried over directly from the original rather than normalized to a plain 0-based
range:

- **SID filter cutoff** (`hb_sid_chip_t.filter_lo/hi`): initialized to
  `0x8000 | (cutoff_init << 5)`.
- **SID pulse width** (`hb_sid_channel_t.sid_pw_lo/hi`): initialized to
  `0x8000 | (pwm_start << 4)`.

The `0x8000` bit isn't arithmetically meaningful — the real SID hardware registers
for both of these are less than 16 bits wide (filter cutoff is 11 bits, pulse
width is 12 bits), and the high bits above that range are simply **ignored by the
SID chip itself** when the value is written directly to the hardware register (no
masking needed on write — confirmed against the SID datasheet's register bit
widths). The marker bit's only real purpose is letting arithmetic like "clamp to a
top/bottom bound" and "check if a value is populated" work with ordinary unsigned
16-bit comparisons on values that would otherwise sit awkwardly close to the
sign/zero boundary. When reading these fields directly (bypassing the public API),
mask off the top bits explicitly rather than assuming a 0-based range.

---

## 8. Verification Methodology

Three independent verification techniques were used throughout this port, in
increasing order of confidence:

1. **Disassembly inspection** (`-g` builds + `.asm`/`.map` output) — confirms the
   compiler generated the code that was intended, catches Oscar64 codegen/
   optimizer issues (see [`oscar64manual.md`](oscar64manual.md) for the specific
   `-O2` bugs found during this port), and is the only way to do the zero-page gap
   analysis in [§3](#3-irq--tick-architecture). Does **not** catch logic bugs that
   compile correctly but implement the wrong thing.
2. **Non-audio hardware verification** — reading the C-level shadow-state structs
   (`hb_sids[]`, `hb_ua[]`, `hb_state`) via live memory reads on real hardware,
   cross-checked against independently-recomputed expected values (e.g. from a
   `dd`/`od` + Python dump of the actual `.reu` file's bytes). This catches real
   logic bugs — it's how a Phase 1 `INSTPARAMS` struct-layout bug (wave/arp table
   at the wrong offset) was found, since the "wrong" bytes decoded to a
   recognizable ASCII instrument name instead of a real waveform value. **Does
   not catch threshold-effect bugs** — a register that's silently stuck at 0 looks
   identical to a channel that's legitimately idle at that row in a raw memory
   dump; only a real listening test caught the missing-PWM-init bug in Phase 9.
   When re-reading memory across multiple deploy/run cycles, always confirm the
   `.prg` currently running on the device is the one just built (rebuild a matching
   `-g` binary with the same version string and `cmp` it byte-for-byte against
   what was deployed) — reading a stale or different program's memory at the
   "right" address looks like plausible-but-wrong data, not an obvious error.
3. **Real listening tests** — the only check that catches threshold effects
   (silence from a 0%-duty-cycle PWM register), tempo/timing feel, and anything
   else that "looks correct in a memory dump" can hide. Several real bugs in this
   port (missing PWM/filter/vibrato init, the turbo-not-engaged tempo bug) were
   only found this way, after passing both of the above checks cleanly.

---

## 9. Build System Overview

Oscar64 follows `#pragma compile("hbplayer.c")` from `hbplayer.h`, so `#include
"hbplayer.h"` is sufficient anywhere the library is used — no separate compile
step. See the Makefile's `ALLSRCS` for the full transitive source list make needs
to track for rebuild correctness (Oscar64 itself resolves `#pragma compile` chains
at compile time; `make` just needs to know when to re-invoke Oscar64 at all).

See [`README.md`](README.md#building-from-source) for the day-to-day build/deploy
commands.
