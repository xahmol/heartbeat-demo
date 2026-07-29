/*****************************************************************
Heartbeat Soundtracker player — C/Oscar64 port, implementation
See hbplayer.h for API documentation and NOTICE.md for attribution.

Phase 1 (current): embedded tables + data structures only. No player
logic yet — see /home/xahmol/.claude/plans/ok-now-plan-for-rosy-lighthouse.md
for the full phasing.
******************************************************************/

#include "hbplayer.h"

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
