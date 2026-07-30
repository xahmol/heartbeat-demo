# Third-Party Code Notice

## Heartbeat Soundtracker Player

`reference/heartbeat-player-src/` (local-only, gitignored — not published to GitHub)
contains the standalone player source code for **Heartbeat Soundtracker**
(https://sites.google.com/view/heartbeatsoundtracker), © Aleksi Eeben / Eight Bit Shed.

This source is normally distributed only to Composer's License / Collector's Edition
customers. Xander Mol holds a Gold license for Heartbeat Soundtracker and obtained
explicit written permission from the author (email correspondence with Aleksi Eeben,
2026) to:

- redistribute this player source as part of this project, including in a public
  repository with public source, and
- create and publish an Oscar64 C library derived from it.

By the project owner's choice, only the resulting C conversion
(`include/hbplayer.h`/`.c`) is published in this public repository — the original
assembly source itself is kept local-only for porting reference, even though
redistributing it is permitted.

Any code in this project ported or adapted from `player.s` / `main.s` / `buttons.s`
must carry a credit comment per the Code Attribution convention in this project's
`CLAUDE.md`, naming Aleksi Eeben / Heartbeat Soundtracker as the original author.

## Song Files (`assets/*.reu`)

**Neither song file in this repository is covered by this project's own GPL-3
license.** Both are covers/arrangements of pre-existing commercial music, kept
here only as demo content for this player, under the terms below — not as
original or GPL-3-licensed works.

- **`Knight Rider Theme.reu`** — a Heartbeat Soundtracker arrangement of the
  *Knight Rider* TV theme (composed by Glen A. Larson / Stu Phillips; ©
  Universal Studios). This specific `.reu` is bundled as example/demo content
  with Heartbeat Soundtracker's own public evaluation release, so it is
  already publicly distributed by the tracker's author — included here on
  that same basis, as demo material for this player, not as an original
  composition of this project.
- **`maniac.reu`** — Xander Mol's own Heartbeat Soundtracker arrangement of
  "Maniac" (Michael Sembello, from *Flashdance*, 1983). Also demo content,
  not an original composition, and not GPL-3-licensed.

If you fork or reuse this repository's own code (the C player port, the demo
itself), these two files are not part of that grant — replace them with your
own composition or a song you have the rights to distribute.
