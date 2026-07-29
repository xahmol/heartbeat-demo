/*****************************************************************
Heartbeat Soundtracker player — C/Oscar64 port, implementation
See hbplayer.h for API documentation and NOTICE.md for attribution.

Phases 1-3 done: data structures, song loader, NTSC detection, init/tempo/
stop-all. No tick IRQ yet (Phase 4) -- see
/home/xahmol/.claude/plans/ok-now-plan-for-rosy-lighthouse.md for phasing.
******************************************************************/

#include <c64/cia.h>
#include <string.h>
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

// ---------------------------------------------------------------
// hb_detect_ntsc — port of main.s's DetectNTSC raster-line timing check.
//
// Only the detection test itself is ported. The original also patches its
// BPM/frequency tables in place once NTSC is detected (converting them
// permanently) — this port deliberately avoids that: hb_set_tempo() and
// the (Phase 9) frequency lookup select between the PAL/NTSC embedded
// tables via this flag at each use instead, so no runtime mutation of
// #embed-sourced data is needed (see port plan §4).
//
// Written as an inline __asm block (not re-derived in C) because the
// timing-sensitive raster-line poll is easy to subtly break via re-
// expression in C; ported instruction-for-instruction instead. Verified
// against the reference algorithm and Oscar64's inline-asm label/branch
// support with a standalone test build (2026-07-29) — see oscar64manual.md.
//
// IMPORTANT: the asm block stores its result into the file-scope static
// hb_ntsc_probe below, NOT a local variable. A local (even `volatile`, even
// routed through a __noinline barrier call) gets silently treated as
// compile-time-constant 0 by Oscar64's optimizer at -O2 — confirmed via a
// standalone test build showing the final `hb_state.x = local;` compiles to
// an unconditional `LDA #$00 / STA ...`, discarding the asm block's actual
// computed value entirely. Writing directly to a static/global from inside
// the __asm block does not have this problem (verified: real STA-then-LDA
// round trip in the generated .asm). See oscar64manual.md's gotcha list —
// this is a third confirmed instance of Oscar64 not tracking inline-asm
// side effects for its dataflow analysis, distinct from (but same family
// as) the reu_count_pages() bug fixed in detect.c.
// ---------------------------------------------------------------
static unsigned char hb_ntsc_probe;

char hb_detect_ntsc(void)
{
    __asm {
        php
        sei
    ntsc_w1:
        lda $d012
    ntsc_w2:
        cmp $d012
        beq ntsc_w2
        bmi ntsc_w1
        plp
        and #$03
        cmp #$03
        bne ntsc_is
        lda #$00
        jmp ntsc_fin
    ntsc_is:
        lda #$01
    ntsc_fin:
        sta hb_ntsc_probe
    }

    hb_state.ntsc_detected = hb_ntsc_probe;
    return hb_ntsc_probe;
}

// ---------------------------------------------------------------
// Init helpers — split to match the original's fall-through structure:
// InitUltimateAudioAndSIDs (UA channels + falls through into SID reset)
// vs. InitSIDImageAndVolumes alone (what StopAllSound jumps directly to,
// deliberately skipping the UA reset -- see port plan discussion; this is
// the reference player's actual behavior, ported as-is for exactness).
// ---------------------------------------------------------------
static void hb_init_sid_image_and_volumes(void)
{
    unsigned char i;

    memset(hb_sids, 0, sizeof(hb_sids));
    for (i = 0; i < HB_MAX_SIDS; i++)
    {
        hb_sids[i].addr = (unsigned)hb_songdata.sid_addresses[i * 2] |
                           ((unsigned)hb_songdata.sid_addresses[i * 2 + 1] << 8);
        hb_sids[i].volume = hb_songdata.sid_volumes[i];
    }
}

static void hb_init_ua_and_sids(void)
{
    unsigned char ch;

    memset(hb_ua, 0, sizeof(hb_ua));
    for (ch = 0; ch < HB_UA_CHANNELS; ch++)
    {
        volatile unsigned char *base =
            (volatile unsigned char *)(unsigned long)audio_ch_base[ch];
        base[AUDIO_OFF_VOL] = 0;
        base[AUDIO_OFF_CTR] = 0;
    }

    hb_init_sid_image_and_volumes();
}

// ---------------------------------------------------------------
// hb_set_tempo — port of SetTempo. Reprograms CIA1 Timer A from the
// embedded BPM table (not a formula -- the reference deliberately uses a
// precomputed PAL table); applies the NTSC delta additively when
// hb_state.ntsc_detected, instead of the original's one-time in-place
// table mutation (see port plan §4).
//
// php/plp (not sei/cli) to match the original exactly: this may be called
// from within the (Phase 4) tick IRQ via a Bt track command, where
// unconditionally re-enabling interrupts at the end (cli) would be wrong.
// ---------------------------------------------------------------
void hb_set_tempo(unsigned char bpm_minus_64)
{
    unsigned char lo, hi;

    __asm { php }
    __asm { sei }

    hb_state.tempo = bpm_minus_64;
    hb_state.tempo_ticks = HB_BPM_TEMPO_TICKS(bpm_minus_64);

    lo = HB_BPM_TIMER_LO(bpm_minus_64);
    hi = HB_BPM_TIMER_HI(bpm_minus_64);
    if (hb_state.ntsc_detected)
    {
        unsigned char old_lo = lo;
        lo = (unsigned char)(lo + hb_bpm_ntsc_add[bpm_minus_64]);
        if (lo < old_lo)   // unsigned add wrapped -> carry into hi
            hi++;
    }
    cia1.ta = (unsigned)lo | ((unsigned)hi << 8);

    __asm { plp }
}

// ---------------------------------------------------------------
// hb_stop_all — port of StopAllSound. Matches the reference exactly:
// only resets SID working state (jmp InitSIDImageAndVolumes in the
// original skips the UA channel reset entirely -- ported as-is, not
// "fixed", since bit-exact parity with the reference player is the goal).
// ---------------------------------------------------------------
void hb_stop_all(void)
{
    hb_state.play_mode = 0;
    hb_init_sid_image_and_volumes();
}

// ---------------------------------------------------------------
// hb_tick — port of PlayerUpdate. PHASE 4: no-op stub (just decrements
// Tick) so the IRQ trampoline/vector plumbing can be verified in
// isolation before the real per-row logic (row fetch, modulations,
// register flush) lands in Phases 5-9.
//
// __interrupt: Oscar64 auto-saves whatever ZP it statically determines
// this function's call tree touches. Re-verify via -g build + .asm
// whenever this body grows -- do NOT assume the trampoline's manual
// save requirements (currently none) still hold once this stops being
// a trivial leaf function. See modplay.c's hb_irq-equivalent
// (modplay_irq) for the full methodology this must be re-run with.
// ---------------------------------------------------------------
// Phase-4-only: free-running, never-clamped fire counter for hardware
// verification (hb_state.tick itself starts small and hits 0 within
// milliseconds -- not usable to prove "still firing" over human-visible
// time). Remove once Phase 5's real per-row logic makes this moot.
unsigned int hb_debug_tick_count;

__interrupt void hb_tick(void)
{
    hb_debug_tick_count++;

    if (hb_state.tick > 0)
        hb_state.tick--;
}

// ---------------------------------------------------------------
// hb_irq — port of PlayerIRQ. Raw __asm entry point (no C prologue),
// installed at $0314/$0315 by hb_init(). Mirrors the reference exactly:
// checks $D019 bit0 first to tell a VIC raster IRQ apart from the CIA1
// Timer A IRQ that actually drives the player tick.
//
// The reference enables BOTH a raster IRQ (fixed line, independent of
// tempo) and the CIA1 Timer A IRQ (tempo-dependent tick rate) at the same
// time: the raster IRQ's only job is guaranteed ~50Hz keyboard scanning
// (SCNKEY) regardless of how slow the current BPM's tick rate is, while
// the CIA branch runs the real player update. The reference chains both
// branches to $EA81 (a KERNAL entry point *past* its own internal SCNKEY
// call, skipping a double-scan against the raster branch's manual SCNKEY).
//
// PORT DEVIATION (hardware-tested, 2026-07-29): chaining to $EA81 here
// causes hb_tick to stop being invoked after only ~2 firings (confirmed via
// a free-running debug counter that should climb continuously but instead
// plateaus within milliseconds) -- with no crash and the keyboard still
// responding, consistent with hitting an unprotected KERNAL/BASIC-ROM-
// dependent path under this project's MMAP_NO_BASIC memory config that
// isn't covered by main.c's existing $0310/$A002 UDTIM/CBINV safety
// patches (those patches were written for -- and documented against --
// $EA31's specific call chain, per main.c's own comments, not $EA81's).
// Switched both branches to $EA31 instead, matching the already-proven,
// already-hardware-verified pattern this project's main.c is designed
// around (see UltimateDemo2026/include/modplay.c's modplay_irq, which
// uses the same target). Accepted tradeoff: both branches now scan the
// keyboard (redundant, but harmless -- cheap at 64 MHz turbo), so the
// manual SCNKEY call in the raster branch is removed (chaining to $EA31
// already does it).
//
// Per the "named asm blocks are addresses, not callables" rule (see
// oscar64manual.md), this is installed via `*((void**)0x0314) = hb_irq;`,
// never called with hb_irq().
// ---------------------------------------------------------------
__asm hb_irq
{
    lda $d019
    and #$01
    bne hb_irq_raster

    jsr hb_tick
    lda $dc0d           // read CIA1 ICR -- acknowledges Timer A IRQ (clear-on-read)
    jmp $ea31

hb_irq_raster:
    sta $d019           // acknowledge raster IRQ
    jmp $ea31           // $EA31's own sequence includes SCNKEY
}

// ---------------------------------------------------------------
// hb_init — port of PlayerInit. Installs the tick IRQ (Phase 4) and
// resets player state. Call hb_detect_ntsc() before this.
// ---------------------------------------------------------------
void hb_init(unsigned char seq_start_pos, unsigned char play_mode)
{
    __asm { sei }

    *((void **)0x0314) = hb_irq;

    *((volatile unsigned char *)0xD01A) = 0x01;   // enable VIC raster IRQ
    *((volatile unsigned char *)0xD019) = 0x01;   // ack any pending raster IRQ
    cia1.cra = 0x01;                               // start CIA1 Timer A (continuous)
    cia1.icr = 0x81;                               // enable CIA1 Timer A IRQ

    // Raster compare line 0: $D011=$1B is the standard VIC text-mode
    // control byte (RSEL/DEN/YSCROLL) with bit7 (raster-line MSB) = 0;
    // $D012=$00 gives the low 8 bits -> line (0<<8)|0 = 0. (NOT "line 27" --
    // $1B is a control-register value here, not a raw line number.)
    *((volatile unsigned char *)0xD011) = 0x1B;
    *((volatile unsigned char *)0xD012) = 0x00;

    hb_init_ua_and_sids();

    hb_set_tempo(hb_songdata.starting_tempo);

    hb_state.patt_length = hb_songdata.song_pattern_length;
    hb_state.patt_step = (unsigned char)(hb_state.patt_length - 1);

    hb_state.tick = (unsigned char)(hb_songdata.hardrestart_time + 3);

    hb_state.last_ua_mutes = 0x7f;
    hb_state.last_sid_mutes = 0xff;

    hb_state.patt_ptr = 0;
    hb_state.patt_bank = 0;
    hb_state.transpose_now = 0;

    hb_state.seq_start_pos = seq_start_pos;
    hb_state.seq_step = (unsigned char)(seq_start_pos - 1);

    hb_state.play_mode = play_mode;

    __asm { cli }
}
