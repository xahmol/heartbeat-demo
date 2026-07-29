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
permission to redistribute the player source publicly and to publish an Oscar64 library
derived from it — see `NOTICE.md` for the full attribution requirement. The reference
source therefore lives in-repo (not gitignored) at `reference/heartbeat-player-src/`.

## Build

```
make          # compile → build/heartbeat-demo.prg
make clean    # remove build artefacts
make deploy   # wput to ULTHOST (set in .env, copy from .env.example)
```

The Makefile sets `-i=include -tm=c64 -tf=prg -O2 -dNOFLOAT`. Oscar64 follows
`#pragma compile` chains from `src/main.c` automatically — no per-file compilation needed.

## Source layout

| Path | Role |
|------|------|
| `src/main.c` | Entry point; currently runs hardware detection sequence only |
| `src/screen.h/.c` | CharWin screen helpers (header, result lines, error exit) |
| `src/detect.h/.c` | Hardware detection: UCI, REU size, turbo, Ultimate Audio |
| `include/defines.h` | Project-wide constants: PETSCII codes, screen codes, colour palette (`COL_*`), string limits, `CharWin cw` extern, `APP_NAME` |
| `include/` | Reusable library headers/sources (turbo, audio, UCI) |
| `build/` | Compiler output (`.prg`, `.map`, `.asm`, `.lbl`) |
| `reference/heartbeat-player-src/` | Heartbeat Soundtracker standalone player 6502 source (from the gold license, redistribution permitted — see `NOTICE.md`), kept in-repo for porting reference — see below |

This repo currently has **no visual effects or Heartbeat playback yet** — it is a
buildchain scaffold only. Hardware detection (`src/detect.c`) confirms UCI, 16 MB REU,
turbo, and Ultimate Audio are present and working before any porting work begins.

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

The standalone player source lives in `reference/heartbeat-player-src/` (in-repo,
redistribution explicitly permitted by the author — see `NOTICE.md`). It was obtained
via a gold license for Heartbeat Soundtracker and mirrors
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

**Porting approach (for future planning, not yet started):** the cleanest path is
likely a hybrid — keep tight per-tick 6502 routines (`RegisterUpdate`, `WriteOneSID`,
frequency table lookups) as inline `__asm` where C introduces too much overhead, but
port control flow, initialization, and one-shot logic (`PlayerInit`, `SetTempo`,
`PlayFX`/`StopFX`, pattern-row parsing) to C. Decide this properly once profiling
shows where 6502 cycles actually matter — do not port everything to `__asm` speculatively.

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
