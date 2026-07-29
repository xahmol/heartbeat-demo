/*****************************************************************
Heartbeat Soundtracker player — C/Oscar64 port, implementation
See hbplayer.h for API documentation and NOTICE.md for attribution.

Phase 1 (current): embedded tables + data structures only. No player
logic yet — see /home/xahmol/.claude/plans/ok-now-plan-for-rosy-lighthouse.md
for the full phasing.
******************************************************************/

#include "hbplayer.h"
#include "audio.h"
#include "ultimate_common_lib.h"
#include "ultimate_dos_lib.h"

// ---------------------------------------------------------------
// Embedded lookup tables — sourced from the Heartbeat player's own
// binary tables (reference/heartbeat-player-src/bin/, gitignored/local-
// only per NOTICE.md; the .bin files themselves are required locally to
// build this file — see CLAUDE.md's build prerequisites note).
//
// #embed must be the only thing on its source line (Oscar64 gotcha —
// merging onto one line mis-tokenizes the embedded byte stream).
// ---------------------------------------------------------------

// BPM table: 768 bytes = 3 x 256-entry pages (tempo_ticks, timer_lo, timer_hi)
static const unsigned char hb_bpmtable[768] = {
    #embed "../reference/heartbeat-player-src/bin/bpmtable.bin"
};
#define HB_BPM_TEMPO_TICKS(bpm)  (hb_bpmtable[(unsigned char)(bpm)])
#define HB_BPM_TIMER_LO(bpm)     (hb_bpmtable[256 + (unsigned char)(bpm)])
#define HB_BPM_TIMER_HI(bpm)     (hb_bpmtable[512 + (unsigned char)(bpm)])

// NTSC BPM timer-lo delta (additive, applied at hb_set_tempo() time — see
// hbplayer.h / the port plan for why this isn't pre-baked into a second table)
static const unsigned char hb_bpm_ntsc_add[256] = {
    #embed "../reference/heartbeat-player-src/bin/bpmtable-ntsc.bin"
};

// Ultimate Audio frequency table (lowest octave), 1536 bytes = 6 x 256-entry
// pages. Clock-independent (6.25MHz UA reference) — no NTSC variant needed.
static const unsigned char hb_ultfreq[1536] = {
    #embed "../reference/heartbeat-player-src/bin/ult-freq.bin"
};

// SID frequency table (highest octave), PAL — 1536 bytes, 6 x 256-entry pages.
static const unsigned char hb_palfreq[1536] = {
    #embed "../reference/heartbeat-player-src/bin/pal-freq.bin"
};

// SID frequency table, NTSC variant — selected instead of hb_palfreq when
// hb_state.ntsc_detected (runtime pointer choice, no in-place mutation of
// the PAL table as the original assembly does — see port plan §4).
static const unsigned char hb_ntscfreq[1536] = {
    #embed "../reference/heartbeat-player-src/bin/ntsc-freq.bin"
};

// ---------------------------------------------------------------
// Global state
// ---------------------------------------------------------------

hb_songdata_t     hb_songdata;
hb_state_t        hb_state;
unsigned char     hb_row_buf[64];
hb_sid_chip_t     hb_sids[HB_MAX_SIDS];
hb_ua_channel_t   hb_ua[HB_UA_CHANNELS];

// ---------------------------------------------------------------
// Phase-1-only debug helper — see hbplayer.h
// ---------------------------------------------------------------
void hb_debug_table_bytes(unsigned char out[5]) {
    out[0] = hb_bpmtable[0];
    out[1] = hb_bpm_ntsc_add[0];
    out[2] = hb_ultfreq[0];
    out[3] = hb_palfreq[0];
    out[4] = hb_ntscfreq[0];
}

// ---------------------------------------------------------------
// hb_load — scan SD/USB drives for `filename`, load it into REU at
// `reu_addr`, then pull the song-data header blob into hb_songdata.
//
// The reference standalone player (main.s) does NOT do this itself — it
// assumes the operator has pre-loaded the .reu file into REU via the U64's
// own file browser before running. This UCI-based loader is a deliberate
// addition so the demo can load its own song, reusing the exact
// scan/open/chunked-load pattern already proven in
// UltimateDemo2026/src/main.c (drive scan) and
// UltimateDemo2026/include/modplay.c's modplay_load (chunked uii_load_reu).
// ---------------------------------------------------------------
char hb_load(char *filename, unsigned long reu_addr)
{
    unsigned long cur_addr;
    unsigned i;
    char found = 0;

    // Install directory to search across SD/USB drives — mirrors the
    // Makefile's INSTALL_PATH exactly (see Makefile's own note about
    // keeping this in sync). Identity charmap override: petscii.h's global
    // charmap (in effect by the time this file compiles, since main.c
    // includes it earlier in the #pragma compile chain) would otherwise
    // remap these path bytes to the wrong PETSCII case for UCI's raw-ASCII
    // filesystem protocol.
    #pragma charmap(97, 97, 26)   // a-z -> a-z (identity)
    #pragma charmap(65, 65, 26)   // A-Z -> A-Z (identity)
    static char hb_install_path[] = "idi8b/heartbeat-demo/";
    #pragma charmap(97, 65, 26)   // restore petscii.h: a-z -> A-Z
    #pragma charmap(65, 97, 26)   // restore petscii.h: A-Z -> a-z

    // Fast path: try the U64's configured home directory first.
    uii_change_dir_home();
    uii_open_file(0x01, filename);
    if (UII_SUCCESS)
        found = 1;

    // Full scan fallback: search all SD and USB drives for the install path.
    if (!found)
    {
        char media_drives[UII_MAX_DRIVES][UII_DRIVE_PATH_LEN];
        char media_count = 0;
        char found_path[UII_DRIVE_PATH_LEN + sizeof(hb_install_path)];

        uii_scan_media(media_drives, &media_count);
        if (uii_find_media_path(media_drives, media_count, hb_install_path, found_path))
        {
            uii_open_file(0x01, filename);
            if (UII_SUCCESS)
                found = 1;
        }
    }

    if (!found)
        return 0;

    // Load in 32767-byte chunks (max safe 16-bit size). 512 iterations
    // covers the full 16 MB REU; uii_load_reu is a no-op past EOF (per
    // modplay_load's proven pattern), and we break as soon as a call fails.
    cur_addr = reu_addr;
    for (i = 0; i < 512; i++)
    {
        uii_load_reu(cur_addr, 32767UL);
        cur_addr += 32767UL;
        if (!UII_SUCCESS)
            break;
    }

    // Pull the $2000-byte song-data header blob into hb_songdata. Fixed
    // offset within the .reu image (see main.s / HB_SONGDATA_REU_SRC_DEFAULT_OFFSET);
    // the .reu file is a flat REU memory snapshot, so this offset is
    // relative to reu_addr, not an absolute REU address.
    hb_state.reu_song_base = reu_addr + HB_SONGDATA_REU_SRC_DEFAULT_OFFSET;
    reu_fetch(&hb_songdata, hb_state.reu_song_base, (unsigned)HB_SONGDATA_SIZE);

    return 1;
}
