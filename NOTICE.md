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
