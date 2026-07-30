# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**heartbeat-demo** is a C demo project targeting the **Ultimate 64** (U64) hardware,
compiled with the **Oscar64** cross-compiler. It uses the U64's turbo mode, 16 MB REU,
and the Ultimate Audio DMA layer — the same advanced-feature set as its sister project
`UltimateDemo2026`, which this repo was scaffolded from.

The goal of this demo is to showcase a song composed in the **Heartbeat Soundtracker**
(https://sites.google.com/view/heartbeatsoundtracker), a tracker built specifically
around the Ultimate 64's Ultimate Audio + REU + SID combination. A gold license for
Heartbeat Soundtracker was purchased, which includes the standalone player's 6502
assembly source. The long-term aim is to port as much of that player as reasonably
possible to **C** as an Oscar64 library, so any Oscar64/U64 project can play a Heartbeat
song without hand-written assembly.

The Heartbeat Soundtracker author (Aleksi Eeben / Eight Bit Shed) has explicitly given
permission to redistribute the player source and to publish an Oscar64 library derived
from it — see `NOTICE.md` for the full attribution requirement. By the project owner's
choice, only the C conversion (`include/hbplayer.h/.c`) is published; the original
assembly reference source is kept **local-only** at `reference/heartbeat-player-src/`
(gitignored — see `.gitignore`), not pushed to GitHub.

## Build

```
make          # compile + regenerate README.pdf + versioned release ZIP (default target)
make clean    # remove build artefacts
make docs     # regenerate README.pdf alone (requires pandoc)
make deploy   # wput PRG + both bundled songs to ULTHOST (set in .env, copy from .env.example)
```

The Makefile sets `-i=include -tm=c64 -tf=prg -O2 -dNOFLOAT`. Oscar64 follows
`#pragma compile` chains from `src/main.c` automatically — no per-file compilation needed.

## Source layout

| Path | Role |
|------|------|
| `src/main.c` | Entry point: hardware detection, song load, then hands off to the visualiser |
| `src/screen.h/.c` | CharWin screen helpers for the detection screen (header, result lines, error exit) |
| `src/detect.h/.c` | Hardware detection: UCI, REU size, turbo, Ultimate Audio |
| `src/visualizer.h/.c` | Note visualiser + test harness screen (VIC bank 2): two-column VU meters, plasma, spectroscope, sprite scrolltext, song switching |
| `include/defines.h` | Project-wide constants: PETSCII codes, screen codes, colour palette (`COL_*`), string limits, `CharWin cw` extern, `APP_NAME` |
| `include/` | Reusable library headers/sources (turbo, audio, UCI, the Heartbeat player itself) |
| `build/` | Compiler output (`.prg`, `.map`, `.asm`, `.lbl`) |
| `reference/heartbeat-player-src/` | **Gitignored, local-only.** Heartbeat Soundtracker standalone player 6502 source (from the gold license; redistribution permitted per `NOTICE.md`, but kept unpublished by choice — only the C conversion is public) — see below |

**Status: v1.0.0, feature-complete and hardware-verified** — full song playback
(all SID + Ultimate Audio channels, modulation, in-pattern track commands), a
`buttons.s`-equivalent test harness, and a "wow factor" note visualiser (two-column
VU meters, plasma, spectroscope, sprite scrolltext, in-demo song switching).
Hardware detection (`src/detect.c`) confirms UCI, 16 MB REU, turbo, and Ultimate
Audio are present and working before playback starts.

For the player library's public API and the song file format, see
[`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md). For internal design (tick/
IRQ architecture, data flow, zero-page verification methodology), see
[`ARCHITECTURE.md`](ARCHITECTURE.md) — read that before making any change to
`hbplayer.c`'s tick-reachable call tree, since it documents a real, recurring
zero-page hazard (`$02`/`mul16by8`) and the exact method used to re-verify it
whenever that call tree changes. Before making any change to the memory layout
(new large globals, moving the visualiser's screen/charset/sprite addresses),
see the region pragmas' own extensive comment in `src/main.c` — a real bug was
found and fixed there (Oscar64's heap/stack silently claim the entire remaining
region tail regardless of declared size, so a naive single-region layout leaves
no genuinely free space for the visualiser to use).

## Toolchain: Oscar64

See `oscar64manual.md` (canonical copy, kept in sync with
`/home/xahmol/git/UltimateDemo2026/oscar64manual.md` per global `~/.claude/CLAUDE.md`
instructions) for the full compiler reference: flags, pragmas, language extensions,
library APIs, gotchas.

All project headers use `#pragma compile("filename.c")` so only the `.h` needs to be
`#include`d; Oscar64 automatically compiles the `.c`.

## Ultimate 64 hardware layers already wired up

These are carried over unmodified from `UltimateDemo2026` (same U64, same Oscar64
target, no changes needed):

| Header | Purpose | Manual |
|--------|---------|--------|
| `include/turbo.h/.c` | U64 turbo speed control and detection | `TURBOCONTROLMANUAL.md` |
| `include/audio.h/.c` | Ultimate Audio hardware layer: 7-channel DMA voices, REU DMA | `ULTIMATEAUDIOMANUAL.md` |
| `include/ultimate_common_lib.h/.c` | UCI core: detection, send/receive protocol engine | `UCILIBMANUAL.md` |
| `include/ultimate_dos_lib.h/.c` | UCI file I/O, directory navigation, disk mounting, REU transfer | `UCILIBMANUAL.md` |

Not carried over from UltimateDemo2026: `modplay.h/.c` (ProTracker MOD player — not
needed, Heartbeat has its own player format) and `ultimate_network_lib.h/.c` /
`ultimate_time_lib.h/.c` (not needed for this demo's scope).

### Required U64 firmware settings

Same as `UltimateDemo2026`, plus the settings documented in
`reference/heartbeat-player-src/Heartbeat.cfg` (SID addressing, filter curves, mixer
levels) which the Heartbeat editor itself expects:

- **Turbo Mode**: F2 → Turbo Mode → *U64 Turbo Registers* (Heartbeat's own player
  boots at 16 MHz turbo — see `main.s` in the reference source)
- **REU**: F2 → C64 settings → REU → *16 MB* (song/pattern/sample data streams from REU)
- **Ultimate Audio**: F2 → C64/Cart settings → *Audio*, mapped at `$DF20-$DFFF`
- **Command Interface (UCI)**: F2 → UCI Settings → Enable

## Heartbeat Soundtracker player — porting reference

The standalone player source lives in `reference/heartbeat-player-src/` (local-only,
gitignored — see `NOTICE.md` for the redistribution permission and why it's kept
unpublished anyway). It was obtained via a gold license for Heartbeat Soundtracker and mirrors
`D:\Retro\Commodore\Heartbeat Soundtracker\Player Source Code` (accessible from WSL at
`/mnt/d/Retro/Commodore/Heartbeat Soundtracker/Player Source Code`).

Contents:

| File | Role |
|------|------|
| `player.s` | The player itself: `PlayerInit`, `PlayerIRQ`, `PlayerUpdate`, pattern fetch (from REU), SID + Ultimate Audio note triggering, modulations (portamento/vibrato/PWM/filter), track commands. ~3100 lines, Bass 6502 Assembler syntax. |
| `main.s` | Standalone player test harness: turbo/NTSC setup, REU song load, main loop |
| `buttons.s` | Keyboard test harness (play/stop music, trigger samples) |
| `Heartbeat.cfg` | Required Ultimate 64 firmware settings (mixer, SID addressing, turbo, REU, UA) |
| `bin/*.bin` | Frequency and BPM lookup tables (PAL/NTSC/Ultimate Audio), charset — `incbin`'d by `main.s` |
| `Player Source Code.pdf`, `DASM.zip` | Original doc + DASM-syntax converted version |

Key facts for the future C port (see `player.s` header comment for the full API):

- Public API: `PlayerInit`, `StopAllSound`, `SetTempo`, `PlayFX`, `StopFX`.
- Fixed memory layout (relocatable via constants at top of `player.s`):
  `PLAYROUTINE=$1000` (player code), `PLAYERTABLES=$2000` (freq/BPM tables),
  `SOUNDPARAMS=$3800` (live sound parameter state), `SONGDATA=$4000` (song data
  copied in from REU, $2000 bytes).
- Player variables live at fixed zero-page (`$F9-$FE`) and low-RAM (`$03A0-$03FF`)
  addresses — **not** relocatable without editing every reference; a C port needs to
  decide whether to preserve these addresses (simplest, keeps ASM/C interop easy) or
  fully re-layout as C structs.
- Driven by a raster IRQ (`PlayerIRQ`) chained through CIA1 Timer A at BPM rate; the
  player expects to own the IRQ vector at `$0314`/`$0315` and CIA1 Timer A.
- Pattern data streams from REU on the fly via direct REU DMA registers (`$DF00-$DF0A`)
  every row — this is distinct from `ultimate_dos_lib.c`'s REU helpers, which are for
  bulk file transfer, not real-time per-row streaming.
- Song data must be preloaded into REU by the host program (via UCI file load into
  REU, see `ultimate_dos_lib.h`) before calling `PlayerInit`.

**Porting approach actually taken:** everything was ported to plain C, including the
per-tick hot paths (`RegisterUpdate`, `WriteOneSID`, frequency table lookups) —
profiling never showed a need for hand-written `__asm`, and turbo mode gives enough
per-tick cycle budget once actually engaged (see `ARCHITECTURE.md`'s "Timing"
discussion — the one real perf bug found in this project was turbo never being
enabled after `detect_turbo()`'s benchmark reset it to 1 MHz, not anything C-vs-asm
related). The only raw `__asm` in the whole player is the `hb_irq` trampoline itself
(a real dispatch requirement, not a speed optimization) and a couple of
inline-`__asm` reads (`hb_detect_ntsc`'s raster-timing probe).

When porting any routine from `player.s`, add a credit comment per the global
`~/.claude/CLAUDE.md` Code Attribution convention, e.g.:

```c
// Based on Heartbeat Soundtracker standalone player (player.s, PlayerInit).
// Original: Aleksi Eeben, bit.ly/heartbeatsoundtracker (gold license source).
// Adapted: C translation of pattern-row parsing and REU streaming.
```

## Target Platform Notes

- Primary target: C64 (`-tm=c64`), output `.prg`
- Enable turbo in firmware menu: "Turbo Mode" → "U64 Turbo Registers"
- Enable audio in firmware menu: "C64 and cartridge settings" → enable Ultimate Audio at `$DF20`
- Audio and REU share the `$DF00`–`$DFFF` range; ensure cartridge settings don't conflict
