# heartbeat-demo

A C/Oscar64 port of the [Heartbeat Soundtracker](https://sites.google.com/view/heartbeatsoundtracker)
standalone player, for the Ultimate 64 — plus a hardware-verified demo that plays a
real Heartbeat song end-to-end: turbo mode, the 16 MB REU, and all 8 SID chips +
7 Ultimate Audio DMA channels driven from a single tick IRQ.

**Status: feature-complete.** All 10 phases of the port are done and
hardware-verified — full song playback (tempo, all SID + Ultimate Audio channels,
vibrato/PWM/filter sweep/portamento modulation, in-pattern track commands) plus a
`buttons.s`-equivalent interactive test harness.

---

## Contents

1. [Requirements](#requirements)
2. [Installation](#installation)
3. [Test Harness Controls](#test-harness-controls)
4. [Documentation](#documentation)
5. [Memory Map](#memory-map)
6. [Credits](#credits)
7. [Building from Source](#building-from-source)
8. [Reusing the Player Library in Your Own Project](#reusing-the-player-library-in-your-own-project)

---

## Requirements

- **Ultimate 64** (U64 or U64 Elite) with firmware configured as follows:
  - **Turbo Mode** enabled: `F2` → Turbo Mode → *U64 Turbo Registers*
    (the tick IRQ's per-tick modulation work needs turbo headroom — see
    [`ARCHITECTURE.md`](ARCHITECTURE.md#3-irq--tick-architecture))
  - **REU** set to 16 MB: `F2` → C64 settings → REU → *16 MB*
  - **Ultimate Audio** enabled: `F2` → C64/Cart settings → *Audio*
  - **Command Interface (UCI)** enabled: `F2` → UCI Settings → Enable
  - If some SID channels are silent, check `F2` → Audio Mixer → `Vol UltiSid 1`/
    `Vol UltiSid 2` are not `OFF` — see
    [`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md#7-firmware-prerequisites)
- A Heartbeat Soundtracker `.reu` song file on an SD card or USB drive (see
  Installation)

---

## Installation

1. Build (see [Building from Source](#building-from-source)) or download a release
   ZIP.
2. Extract to the **root** of an SD card or USB drive — the ZIP contains the
   `idi8b/heartbeat-demo/` folder already, so extracting at the drive root creates
   the correct layout.
3. Place your Heartbeat Soundtracker `.reu` song file in that same folder,
   named to match `hb_song_file[]` in `src/main.c` (ships set to
   `"Knight Rider Theme.reu"`, the reference test song).
4. Insert the SD card / connect the USB drive to your Ultimate 64.
5. In the Ultimate menu, navigate to `idi8b/heartbeat-demo/` and load
   `heartbeat-demo.prg`.

The demo auto-detects hardware, loads the song (auto-scanning all connected SD/USB
drives if it isn't found at the U64's configured home directory first), and starts
playback automatically.

---

## Test Harness Controls

Once the song starts playing, a `buttons.s`-equivalent interactive test harness is
active:

| Key | Action |
|---|---|
| `SPACE` | Restart the song from the beginning |
| `RUN/STOP` | Silence all sound |
| `A`-`O` | Manually trigger sample FX 1-15 at C-4 on Ultimate Audio channel 6 |
| `X` | Stop the FX channel (6) |
| `RETURN` | Exit the harness, return to BASIC |

If the letter keys don't seem to respond, check that Shift Lock isn't engaged.

---

## Documentation

| Document | Covers |
|---|---|
| [`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md) | Public API reference, song file format, track command reference |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Internal design: tick/IRQ architecture, data flow, shadow-flush pattern, verification methodology |
| [`TURBOCONTROLMANUAL.md`](TURBOCONTROLMANUAL.md) | Turbo speed control library |
| [`ULTIMATEAUDIOMANUAL.md`](ULTIMATEAUDIOMANUAL.md) | Ultimate Audio DMA hardware layer |
| [`UCILIBMANUAL.md`](UCILIBMANUAL.md) | Ultimate Command Interface (file/REU access) library |
| [`oscar64manual.md`](oscar64manual.md) | Oscar64 compiler reference and gotchas encountered during this project |
| [`NOTICE.md`](NOTICE.md) | Third-party licensing (Heartbeat Soundtracker player source) |

A PDF of this README is included in release ZIPs (`README.pdf`, regenerated via
`make docs` — see [Building from Source](#building-from-source)).

---

## Memory Map

Runtime layout for the compiled binary (Oscar64, VIC bank 0, `$01=$36` — KERNAL +
I/O visible, BASIC ROM removed). Exact section sizes shift per build; check
`build/heartbeat-demo.map` for the current binary.

### Program sections

| Range | Contents |
|-------|----------|
| `$0801`-`$0852` | Oscar64 BASIC bootstrap (`SYS` stub) |
| `$0400`-`$07FF` | Text screen RAM (VIC bank 0, 40×25 chars) |
| `$0A00`-`$BFFF` | Code + data + BSS + heap + stack (single build region, see `#pragma region` in `src/main.c`) |

### I/O region (`$D000`-`$DFFF` at `$01=$36`)

| Address | Device |
|---------|--------|
| `$D000`-`$D3FF` | VIC-II registers |
| `$D400`-`$D7FF` | SID registers (up to 8 chips, addresses from the song's own `sid_addresses` table) |
| `$D800`-`$DBFF` | Color RAM |
| `$DC00`-`$DCFF` | CIA 1 (keyboard matrix; Timer A drives the tick IRQ; also a fixed raster IRQ for keyboard scanning independent of tempo) |
| `$DD00`-`$DDFF` | CIA 2 (serial bus; VIC bank select) |
| `$DF00`-`$DF1F` | REU registers |
| `$DF20`-`$DFFF` | Ultimate Audio channels 0-6 |

### Patches applied at startup

| Address | Value | Reason |
|---------|-------|--------|
| `$0310` | `$60` (RTS) | Stub target for KERNAL UDTIM hook redirects |
| `$A002`:`$A003` | `$10 $03` | Redirects KERNAL `JMP ($A002)` to the RTS stub at `$0310` — prevents the KERNAL IRQ chain from calling BASIC ROM code at an address that's now DRAM, not ROM, under `MMAP_NO_BASIC` |

### Player runtime state and embedded tables

See [`ARCHITECTURE.md`](ARCHITECTURE.md#2-memory-layout) for the full breakdown of
`hb_songdata`/`hb_state`/`hb_sids`/`hb_ua` and the embedded BPM/frequency tables.

### REU (16 MB, `$000000`-`$FFFFFF`)

| Range | Contents |
|-------|----------|
| `$000000` + | The loaded `.reu` song file (header, patterns, samples — see [`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md#4-song-data-structures) for the internal layout) |

---

## Credits

- **Code:** Xander Mol
- **Demo framework:** scaffolded from [UltimateDemo2026](https://github.com/xahmol/UltimateDemo2026)
- **Heartbeat Soundtracker player:** © Aleksi Eeben / Eight Bit Shed, used and
  redistributed with permission — see [`NOTICE.md`](NOTICE.md)
- **UCI/DOS library:** Scott Hutter & Francesco Sblendorio
- **Compiler:** [Oscar64](https://github.com/drmortalwombat/oscar64) by drmortalwombat

---

## Building from Source

### Prerequisites

| Tool | Purpose | Install |
|---|---|---|
| [Oscar64](https://github.com/drmortalwombat/oscar64) | C compiler | see project README |
| `zip` | Release archive | `sudo apt install zip` |
| `wput` | FTP deploy | `sudo apt install wput` |
| `curl` | Deploy connectivity check | `sudo apt install curl` |
| `pandoc` + `texlive-xetex` | PDF docs (optional) | `sudo apt install pandoc texlive-xetex` |

### Build

```
make          # compile → build/heartbeat-demo.prg + versioned ZIP in build/
make clean    # remove build artefacts
make docs     # regenerate README.pdf (requires pandoc)
make deploy   # upload PRG + test song to your Ultimate 64 via FTP
```

`make all` (the default target) also regenerates `README.pdf` if `pandoc` is
available, so it stays in sync with `README.md` in every release ZIP.

### Deployment setup

Copy `.env.example` to `.env` and set your Ultimate 64's IP address:

```
cp .env.example .env
```
```
ULTHOST = 192.168.1.x
```

`.env` is listed in `.gitignore` and will not be committed. `make deploy` checks
connectivity first (`check-deploy`) and prints a clear error if the device is
unreachable, instead of failing deep inside `wput`.

### Make targets

| Target | Does |
|---|---|
| `all` (default) | Build `.prg`, regenerate `README.pdf`, produce a versioned release ZIP |
| `clean` | Remove `build/` artefacts |
| `zip` | Build the release ZIP alone |
| `docs` | Regenerate `README.pdf` alone |
| `check-deploy` | Verify the U64 in `.env` is reachable (used internally by `deploy`) |
| `deploy` | Upload the `.prg` and test song to the U64 via FTP |

---

## Reusing the Player Library in Your Own Project

`include/hbplayer.h`/`.c` is a self-contained, reusable library for playing
Heartbeat Soundtracker songs in any Oscar64-based Ultimate 64 project. Copy the
files, `#include "hbplayer.h"`, and Oscar64's `#pragma compile` chain handles the
rest.

**Important:** building `hbplayer.c` requires the licensed Heartbeat Soundtracker
player's binary lookup tables (`reference/heartbeat-player-src/bin/*.bin`), which
are **not** included in this repository — see [`NOTICE.md`](NOTICE.md). You'll
need your own Heartbeat Soundtracker license to obtain them.

| Files | `include/hbplayer.h` / `include/hbplayer.c` |
|-------|---------------------------------------------|
| Manual | [`HEARTBEATPLAYERMANUAL.md`](HEARTBEATPLAYERMANUAL.md) |
| Architecture | [`ARCHITECTURE.md`](ARCHITECTURE.md) |

```c
#include "hbplayer.h"
#include "turbo.h"

turbo_fast();                                    // required -- see ARCHITECTURE.md

if (hb_load("My Song.reu", 0x000000UL)) {
    hb_detect_ntsc();
    hb_init(0, 1);                               // start playing from step 0

    // playback now runs in the background via the tick IRQ
    for (;;) {
        // ... your demo code, hb_play_fx() for one-off effects, etc. ...
    }
}
```
