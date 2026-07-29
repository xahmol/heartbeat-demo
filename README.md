# heartbeat-demo

A demo for the Ultimate 64, built around a song composed in
[Heartbeat Soundtracker](https://sites.google.com/view/heartbeatsoundtracker), showcasing
turbo mode, the 16 MB REU, and Ultimate Audio DMA.

**Status: buildchain scaffold.** No visual effects or Heartbeat playback yet — see
`CLAUDE.md` for the porting plan.

## Requirements

- **Ultimate 64** (U64 or U64 Elite) with firmware configured as follows:
  - **Turbo Mode** enabled: F2 → Turbo Mode → *U64 Turbo Registers*
  - **REU** set to 16 MB: F2 → C64 settings → REU → *16 MB*
  - **Ultimate Audio** enabled: F2 → C64/Cart settings → *Audio*
  - **Command Interface (UCI)** enabled: F2 → UCI Settings → Enable

## Building from source

Requires the [Oscar64](https://github.com/drmortalwombat/oscar64) cross-compiler.

```
make          # compile → build/heartbeat-demo.prg
make clean    # remove build artefacts
make deploy   # copy .env.example to .env, set ULTHOST, then wput to your U64
```

## Credits

- Demo framework scaffolded from [UltimateDemo2026](https://github.com/xahmol/UltimateDemo2026).
- Heartbeat Soundtracker player source © Aleksi Eeben / Eight Bit Shed, used and
  redistributed with permission — see `NOTICE.md`.
