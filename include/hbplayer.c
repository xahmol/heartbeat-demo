/*****************************************************************
Heartbeat Soundtracker player — C/Oscar64 port, implementation
See hbplayer.h for API documentation and NOTICE.md for attribution.

Phases 1-7 done: data structures, song loader, NTSC detection, init/tempo/
stop-all, tick IRQ, pattern-row fetch, SID + UA note trigger with register
flush (SID side simplified -- no PWM/filter/vibrato/arpeggio movement or
track-command dispatch yet, see hb_play_sid_note()'s header comment; UA
side is a full port of PlaySampleNote/StopSampleNote). Track commands
(Phase 8) and full modulation (Phase 9) still outstanding -- see
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

// Forward declarations -- implementations are below hb_fetch_pattern_row()
// (Phases 6-7: SID/UA note trigger + shadow flush). Kept as static
// internals, not part of the public API in hbplayer.h.
static void hb_sid_hard_restart(void);
static void hb_play_pattern_row(void);
static void hb_register_update(void);

// ---------------------------------------------------------------
// hb_tick — port of PlayerUpdate (SID + UA; track commands are Phase 8,
// live modulation -- vibrato/PWM/filter-sweep/wave-arp stepping,
// portamento -- is Phase 9).
//
// Mirrors the original's MusicPlayMode dispatch exactly:
//   bit7 set ($80)      -> return immediately, skip even RegisterUpdate
//                          (matches .alloff jumping past Modulations/
//                          RegisterUpdate entirely)
//   ==0 (idle/ended)    -> skip tick/row logic, but STILL flush registers
//                          (matches .done -- envelope decay etc. must
//                          keep running even after notes stop)
//   otherwise (playing) -> full tick countdown + row-fetch/play dispatch,
//                          then flush registers
//
// __interrupt: Oscar64 auto-saves whatever ZP it statically determines
// this function's call tree touches. Re-verified via -g build + .asm for
// Phase 7's additions (see hb_irq's comment for the full gap-analysis
// history) -- do NOT assume past results still hold once this body grows
// again (Phase 8+).
// ---------------------------------------------------------------
// Phase-4-only: free-running, never-clamped fire counter for hardware
// verification (hb_state.tick itself starts small and hits 0 within
// milliseconds -- not usable to prove "still firing" over human-visible
// time). Kept for now since it's still a cheap, useful liveness check.
unsigned int hb_debug_tick_count;

__interrupt void hb_tick(void)
{
    hb_debug_tick_count++;

    if ((signed char)hb_state.play_mode < 0)
        return; // bit7 set: skip everything, including register update

    if (hb_state.play_mode != 0)
    {
        hb_state.tick--;

        if (hb_state.tick == 0)
        {
            hb_play_pattern_row(); // resets hb_state.tick at its own end
        }
        else if (hb_state.tick == hb_songdata.hardrestart_time)
        {
            hb_fetch_pattern_row();
            hb_sid_hard_restart();
        }
    }

    hb_register_update();
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
//
// ZERO-PAGE GAP (found 2026-07-29, Phase 6): hb_tick's call tree grew to
// include hb_play_sid_note(), whose `note << 6` frequency-scaling compiles
// to a call to Oscar64's mul16by8 runtime routine (rather than unrolled
// shifts). Verified via -g build + .asm inspection that mul16by8 uses
// zero-page $02 as scratch, in addition to ACCU+0/1 ($1B/$1C) -- and that
// $02 is NOT among the ZP locations Oscar64's __interrupt prologue for
// hb_tick auto-saves (confirmed it saves WORK+0..3, P0-P2, ACCU+0..3, and
// several T-series temporaries, but not $02). A full sweep of every JSR
// target reachable from hb_tick (hb_check_sid_mute, hb_play_sid_note,
// hb_fetch_pattern_row, hb_sid_hard_restart, hb_register_update_sid, and
// their own callees) found only two runtime helpers in total: divmod
// (WORK+0..3/ACCU+0..1, already covered) and mul16by8 ($02, NOT covered).
// Manually saved/restored here around the hb_tick call, following the
// same method as modplay_irq's documented gap analysis in
// UltimateDemo2026/include/modplay.c. Re-run this same sweep whenever
// hb_tick's call tree changes (Phase 7+ will add more).
// ---------------------------------------------------------------
__asm hb_irq
{
    lda $d019
    and #$01
    bne hb_irq_raster

    lda $02
    pha
    jsr hb_tick
    pla
    sta $02
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

// ---------------------------------------------------------------
// hb_fetch_pattern_row — port of FetchPatternRow, through the REU row
// fetch and pattern-pointer advance. Does NOT include SIDHardRestart
// (that lands with Phase 6's SID note-trigger port) -- PHASE 5 scope is
// just the row-fetch/sequencer-advance plumbing, verified by direct
// repeated calls from main.c's debug block, not yet wired into hb_tick's
// real per-tick dispatch (that needs PlayPatternRow/Modulations/
// RegisterUpdate, which don't exist until Phases 6-9).
//
// Pattern REU addressing: each pattern occupies an exact 4096-byte
// (0x1000) REU slot, starting at slot (pattern_number + 15) -- i.e.
// REU address = (pattern_number + 15) << 12. Traced from the original's
// "adc #$0f" then four (asl PattPointerH / rol A) steps starting A=0,
// which computes exactly value*16 split across a 16-bit hi:lo pair.
// 16 patterns exactly tile one 65536-byte REU bank (16 x 4096 = 65536),
// so the bank only ever changes when switching patterns, never mid-
// pattern (64 rows x 64 bytes = 4096 bytes = exactly one slot).
//
// MusicPlayMode==2 (pattern-loop/editor "PatternBuffer" mode) is an
// explicit MVP non-goal (see port plan) -- the fallback below just loops
// the current pattern from row 0 instead of reading the (unimplemented)
// PatternBuffer at $C000.
// ---------------------------------------------------------------
void hb_fetch_pattern_row(void)
{
    unsigned long reu_addr;

    hb_state.patt_step++;

    if (hb_state.patt_step >= hb_state.patt_length)
    {
        // End of pattern: advance sequencer (song play) or loop (fallback).
        if (hb_state.play_mode == 1)
        {
            unsigned char pat;

            hb_state.seq_step++;
            pat = hb_songdata.sequencer_patterns[hb_state.seq_step];

            if (pat == 0)
            {
                hb_state.play_mode = 0;
                memset(hb_row_buf, 0, sizeof(hb_row_buf));
                return;
            }

            if (pat == 0xFF)
            {
                // Loop command: jump to the loop-target step (stored in
                // the transpose table at this step), then re-read the
                // pattern number there.
                hb_state.seq_step = hb_songdata.sequencer_transpose[hb_state.seq_step];
                pat = hb_songdata.sequencer_patterns[hb_state.seq_step];
                if (pat == 0)
                {
                    hb_state.play_mode = 0;
                    memset(hb_row_buf, 0, sizeof(hb_row_buf));
                    return;
                }
            }

            if (pat >= 0x41)
            {
                // Illegal pattern number (or loop target) -> song end.
                hb_state.play_mode = 0;
                memset(hb_row_buf, 0, sizeof(hb_row_buf));
                return;
            }

            reu_addr = ((unsigned long)(pat + 15)) << 12;
            hb_state.patt_bank = (unsigned char)(reu_addr >> 16);
            hb_state.patt_ptr  = (unsigned)(reu_addr & 0xFFFFUL);
            hb_state.patt_step = 0;
            hb_state.transpose_now = (signed char)hb_songdata.sequencer_transpose[hb_state.seq_step];
        }
        else
        {
            // Non-goal fallback (play_mode==2): loop current pattern from row 0.
            hb_state.patt_ptr = 0;
            hb_state.patt_step = 0;
        }
    }

    reu_addr = ((unsigned long)hb_state.patt_bank << 16) | hb_state.patt_ptr;
    reu_fetch(hb_row_buf, reu_addr, 64);
    hb_state.patt_ptr = (unsigned)(hb_state.patt_ptr + 64);
}

// =================================================================
// Phase 6: SID note trigger + shadow flush
// =================================================================

// ---------------------------------------------------------------
// hb_get_sid_freq — port of GetSIDFreq: converts a 16-bit "linear"
// frequency (range $0000-$17FF) into an actual SID frequency register
// value via octave-folding + a per-octave lookup table (hb_palfreq or
// hb_ntscfreq). The original uses self-modifying code (a patched JMP
// target for the shift amount, and a patched LDA operand high byte for
// the table page) -- re-expressed here as ordinary indexing + a shift,
// verified mathematically equivalent:
//   shift = 7 - (Y/3), page = (Y%3)*256   [Y = octave-folded input, see below]
// ---------------------------------------------------------------
static unsigned hb_get_sid_freq(unsigned char freq_hi, unsigned char freq_lo)
{
    unsigned char y = freq_hi;
    unsigned char x = freq_lo;
    unsigned char shift;
    unsigned page;
    const unsigned char *table;
    unsigned v;

    if ((signed char)y < 0)   // negative input -> clamp
    {
        y = 0;
        x = 0;
    }

    while (y >= 0x18)         // fold down octaves until below $17FF
        y = (unsigned char)(y - 3);

    shift = (unsigned char)(7 - y / 3);
    page  = (unsigned)(y % 3) * 256;

    table = hb_state.ntsc_detected ? hb_ntscfreq : hb_palfreq;

    v = ((unsigned)table[page + 0x300 + x] << 8) | table[page + x];
    v >>= shift;
    return v;
}

// ---------------------------------------------------------------
// hb_check_sid_mute — port of CheckSIDMute. Returns 1 if the SID chip at
// sid_idx is muted for the current sequencer step; also force-releases
// its 3 channels' gates (key-up) on the transition INTO mute (matching
// the original's LastSIDMutes comparison), so a note doesn't hang when
// muted mid-play.
// ---------------------------------------------------------------
static char hb_check_sid_mute(unsigned char sid_idx)
{
    unsigned char bit = (unsigned char)(1 << sid_idx);

    if (hb_songdata.sequencer_sidmutes[hb_state.seq_step] & bit)
        return 0; // not muted

    if (hb_state.last_sid_mutes & bit) // was NOT muted last step -> just transitioned
    {
        unsigned char c;
        for (c = 0; c < 3; c++)
            hb_sids[sid_idx].ch[c].gate_mask = 0xFE;
    }
    return 1; // muted
}

// ---------------------------------------------------------------
// hb_sid_hard_restart — port of SIDHardRestart (deferred from Phase 5's
// FetchPatternRow, which this project splits out since it's SID-specific
// register-image work, not row-fetch/sequencer work). For each unmuted
// SID channel with a genuine new note incoming this row (not empty, not
// an effect/command byte, not a stop, not a tied note), forces the
// envelope/waveform/frequency to a clean silent state ahead of the real
// note-on, avoiding audible artifacts from whatever the previous note
// left behind.
// ---------------------------------------------------------------
static void hb_sid_hard_restart(void)
{
    unsigned char sid_idx, ch_idx;

    for (sid_idx = 0; sid_idx < HB_MAX_SIDS; sid_idx++)
    {
        if (hb_check_sid_mute(sid_idx))
            continue;

        for (ch_idx = 0; ch_idx < 3; ch_idx++)
        {
            unsigned char off = (unsigned char)(14 + sid_idx * 6 + ch_idx * 2);
            unsigned char note = hb_row_buf[off];
            hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];

            if (note == 0)                continue; // no note
            if (note & 0x80)               continue; // effect/command, not a note
            if (note == 1)                 continue; // stop note, not hard-restarted
            if (hb_row_buf[off + 1] == 0)  continue; // tied note, no hard restart

            c->sid_env_ad = hb_songdata.hardrestart_ad;
            c->sid_env_sr = hb_songdata.hardrestart_sr;
            c->sid_wave = 0;
            c->wave_arp_count = 0; // instrument inactive
            c->sid_freq_lo = 0;
            c->sid_freq_hi = 0;
            c->gate_mask = 0xFE;   // gate off in key-up mask
        }
    }
}

// ---------------------------------------------------------------
// hb_play_sid_note — SIMPLIFIED port of PlaySIDNote (Phase 6 scope).
//
// Sets up envelope, gate, finetune, portamento speed/target-reached, and
// wave/arp-table-stepping state (step/count/speed reset so Phase 9 starts
// clean), plus an IMMEDIATE frequency conversion (base note + finetune ->
// hb_get_sid_freq), used directly as the active SID frequency register.
//
// Deliberate simplifications (deferred to Phase 9, which doesn't exist
// yet): the original recomputes SIDFreq every TICK in ModulateChannel
// (base + arpeggio + vibrato + finetune) and steps through the wave/arp
// table each tick too -- here the note plays at a fixed pitch (base +
// finetune only) with a fixed waveform (table step 0) and no vibrato/
// arpeggio movement. PWM and filter setup are also explicit Phase 6
// non-goals (deferred to Phase 9 along with their modulation) -- those
// fields stay at whatever hb_init_sid_image_and_volumes() zeroed them to,
// meaning no PWM sweep and no filter for now.
// ---------------------------------------------------------------
static void hb_play_sid_note(unsigned char sid_idx, unsigned char ch_idx, unsigned char note)
{
    hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];
    unsigned char off = (unsigned char)(14 + sid_idx * 6 + ch_idx * 2);
    unsigned char sample_num = hb_row_buf[off + 1];
    unsigned linear = ((unsigned)note) << 6; // BaseFreq/H = note*64 (two lsr/ror pairs in the original)

    if (sample_num == 0)
    {
        // Tied note: only retarget portamento (or jump instantly if speed is 0).
        c->target_freq_lo = (unsigned char)linear;
        c->target_freq_hi = (unsigned char)(linear >> 8);
        if (c->portamento == 0)
        {
            c->base_freq_lo = (unsigned char)linear;
            c->base_freq_hi = (unsigned char)(linear >> 8);
            c->target_freq_hi = 0xFF; // flag: portamento target reached
        }
        return;
    }

    {
        hb_inst_params_t *inst = &hb_songdata.inst_params[sample_num - 1];
        signed char ft = (signed char)inst->finetune;
        unsigned withfinetune, freq;
        unsigned char w;

        c->instr_lo = (unsigned char)(sample_num - 1); // index, not a real pointer (see hbplayer.h)
        c->instr_hi = 1;                                // nonzero = instrument active

        c->sid_env_ad = inst->env_ad;
        c->sid_env_sr = inst->env_sr;

        c->finetune = inst->finetune;
        c->finetune_hi = (ft < 0) ? 0xFF : 0x00; // sign-extend

        c->portamento = inst->portamento;

        c->vib_phase = 0;
        c->vib_frac = 0;
        c->target_freq_hi = 0xFF; // portamento target reached (nothing pending)

        c->wave_arp_speed = 4;
        c->wave_arp_count = 1;
        c->wave_arp_step = 0;
        c->gate_mask = 0xFF; // gate from wave table

        // Phase 6 simplification: static waveform from the wave table's
        // step 0 (Phase 9 steps through this table every tick). Fall back
        // to triangle+gate if step 0 is a command byte ($FC-$FF) or empty.
        w = inst->wave_table[0];
        if (w == 0 || w >= 0xFC)
            w = 0x10;
        c->sid_wave = (unsigned char)(w & c->gate_mask);

        c->base_freq_lo = (unsigned char)linear;
        c->base_freq_hi = (unsigned char)(linear >> 8);

        // Phase 6 simplification: convert base+finetune directly to the
        // active SID frequency register now, instead of every tick via
        // ModulateChannel (no arpeggio/vibrato applied yet -- Phase 9).
        withfinetune = (unsigned)(linear + (((unsigned)(unsigned char)c->finetune) | ((unsigned)c->finetune_hi << 8)));
        freq = hb_get_sid_freq((unsigned char)(withfinetune >> 8), (unsigned char)withfinetune);
        c->sid_freq_lo = (unsigned char)freq;
        c->sid_freq_hi = (unsigned char)(freq >> 8);
    }
}

// ---------------------------------------------------------------
// hb_play_pattern_row_sid — SID portion of PlayPatternRow. The Cmd-channel
// command check (checked first in the original) and per-channel command
// bytes (bit7 set) are Phase 8 scope -- skipped here.
//
// Does NOT update last_ua_mutes/last_sid_mutes or reset hb_state.tick --
// those happen once, in hb_play_pattern_row() below, AFTER both this and
// the UA dispatch (hb_play_pattern_row_ua()) have run. Doing it here
// instead would make the UA loop's mute-transition check always compare
// the current row's mutes against itself instead of the previous row's.
// ---------------------------------------------------------------
static void hb_play_pattern_row_sid(void)
{
    unsigned char sid_idx, ch_idx;

    for (sid_idx = 0; sid_idx < HB_MAX_SIDS; sid_idx++)
    {
        if (hb_check_sid_mute(sid_idx))
            continue;

        for (ch_idx = 0; ch_idx < 3; ch_idx++)
        {
            unsigned char off = (unsigned char)(14 + sid_idx * 6 + ch_idx * 2);
            unsigned char note = hb_row_buf[off];
            hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];

            if (note == 0)
                continue; // no note

            if (note & 0x80)
                continue; // track command (Cmd8x_*) -- Phase 8 scope, no-op for now

            if (note == 1)
            {
                c->gate_mask = 0xFE; // force release (key-up)
                continue;
            }

            {
                unsigned char transposed = (unsigned char)(note + (unsigned char)hb_state.transpose_now);
                if ((signed char)transposed < 0)
                    continue; // transpose bounds check (matches .skip)
                if (transposed < 2)
                    continue;
                hb_play_sid_note(sid_idx, ch_idx, transposed);
            }
        }
    }
}

// ---------------------------------------------------------------
// hb_write_one_sid / hb_register_update_sid — port of RegisterUpdate's
// SID half + WriteOneSID: flush the active register image to real SID
// hardware for every populated chip slot (addr != 0). Filter cutoff's
// internal 16-bit-ish repr ($8000-$9FFF) splits into the SID's 3-bit-lo +
// 8-bit-hi register pair with a bit gap -- verified algebraically
// equivalent to the original's separate lo/hi shift sequences by working
// the derivation through both ways and confirming the same bit positions
// result (see commit history for the derivation).
// ---------------------------------------------------------------
static void hb_write_one_sid(hb_sid_chip_t *chip)
{
    volatile unsigned char *reg = (volatile unsigned char *)(unsigned long)chip->addr;
    unsigned char ch;
    unsigned filt;

    filt = ((unsigned)chip->filter_hi << 8) | chip->filter_lo;
    reg[0x15] = (unsigned char)((filt >> 2) & 0x07);
    reg[0x16] = (unsigned char)(filt >> 5);
    reg[0x17] = chip->filt_ctrl;
    reg[0x18] = chip->volume;

    for (ch = 0; ch < 3; ch++)
    {
        hb_sid_channel_t *c = &chip->ch[ch];
        unsigned char base = (unsigned char)(ch * 7);
        reg[base + 0] = c->sid_freq_lo;
        reg[base + 1] = c->sid_freq_hi;
        reg[base + 2] = c->sid_pw_lo;
        reg[base + 3] = c->sid_pw_hi;
        reg[base + 5] = c->sid_env_ad;
        reg[base + 6] = c->sid_env_sr;
        reg[base + 4] = c->sid_wave; // waveform+gate written last, matches original
    }
}

static void hb_register_update_sid(void)
{
    unsigned char i;
    for (i = 0; i < HB_MAX_SIDS; i++)
        if (hb_sids[i].addr != 0)
            hb_write_one_sid(&hb_sids[i]);
}

// =================================================================
// Phase 7: Ultimate Audio sample trigger + shadow flush
// =================================================================

// ---------------------------------------------------------------
// hb_get_ultimate_freq — port of GetUltimateFreq: the UA counterpart to
// hb_get_sid_freq(), same octave-fold + per-octave table lookup, but two
// differences from the SID version (verified against the source): the
// shift-amount table is in the OPPOSITE order (shift = Y/3, not 7-Y/3),
// and the result gets an extra "-1 with borrow" adjustment at the end
// (a plain 16-bit `v -= 1` in C already wraps/borrows correctly, so no
// separate borrow-flag logic is needed here). Clock-independent (fixed
// 6.25MHz UA reference) -- no NTSC variant, unlike the SID table.
// ---------------------------------------------------------------
static unsigned hb_get_ultimate_freq(unsigned char freq_hi, unsigned char freq_lo)
{
    unsigned char y = freq_hi;
    unsigned char x = freq_lo;
    unsigned char shift;
    unsigned page;
    unsigned v;

    if ((signed char)y < 0)
    {
        y = 0;
        x = 0;
    }

    while (y >= 0x18)
        y = (unsigned char)(y - 3);

    shift = (unsigned char)(y / 3);       // reversed vs hb_get_sid_freq
    page  = (unsigned)(y % 3) * 256;

    v = ((unsigned)hb_ultfreq[page + 0x300 + x] << 8) | hb_ultfreq[page + x];
    v >>= shift;
    v -= 1;
    return v;
}

// ---------------------------------------------------------------
// hb_play_sample_note — port of PlaySampleNote. Unlike SID's frequency
// (which is only READ by hardware once RegisterUpdate flushes SIDImage),
// the UA rate register has no shadow/batch mechanism in the original --
// only the gate/control byte is deferred via UAShadowGate. So this writes
// sample-start/length/loop-point/volume/pan/rate directly to hardware
// immediately (matching the original's own direct pokes), and only
// defers the final gate-trigger byte into hb_ua[].shadow_gate for
// hb_register_update_ua() to flush.
//
// No Phase-9-deferred simplifications here (unlike hb_play_sid_note) --
// PlaySampleNote has no per-tick modulation dependency for its initial
// trigger (arpeggio/vibrato are SID-only concepts); portamento sliding is
// still Phase 9 scope, but the *initial* rate here is already exactly
// what the original computes.
// ---------------------------------------------------------------
static void hb_play_sample_note(unsigned char ua_idx, unsigned char raw_note)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    volatile unsigned char *base = (volatile unsigned char *)(unsigned long)audio_ch_base[ua_idx];
    unsigned char sample_num = hb_row_buf[ua_idx * 2 + 1];
    unsigned char note = (unsigned char)(raw_note + 9); // "note value adjust" (matches PlayFX's own +9)

    if (sample_num == 0)
    {
        // Tied note: retarget portamento only (or jump instantly if speed is 0).
        unsigned char t = (unsigned char)(note + c->note_pitch);
        if (!(c->drum_flag & 0x80))
            t = (unsigned char)(t + (unsigned char)hb_state.transpose_now);
        if ((signed char)t < 0)
            t = 0;

        {
            unsigned linear = ((unsigned)t) << 6;
            c->target_freq_lo = (unsigned char)linear;
            c->target_freq_hi = (unsigned char)(linear >> 8);

            if (c->portamento == 0)
            {
                unsigned rate = hb_get_ultimate_freq((unsigned char)(linear >> 8), (unsigned char)linear);
                c->freq_lo = (unsigned char)linear;
                c->freq_hi = (unsigned char)(linear >> 8);
                c->target_freq_hi = 0xFF; // flag: portamento target reached
                audio_channel_set_rate(ua_idx, rate);
            }
        }
        return;
    }

    {
        hb_sample_params_t *sp = &hb_songdata.sample_params[sample_num - 1];
        unsigned char tempy;
        unsigned linear, rate;
        unsigned char loop_mode;
        unsigned long sample_addr, length;

        c->sample_lo = (unsigned char)(sample_num - 1); // index, not a real pointer
        c->sample_hi = 1;                                // nonzero = sample active

        // Immediate stop -- "note: writing directly to UA register" in
        // the original; this is a real direct poke, not shadow-deferred.
        base[AUDIO_OFF_CTR] = 0x10;
        c->shadow_gate = 0x10;

        if (sp->reu_bank == 0)
            return; // no sample in this slot

        base[AUDIO_OFF_VOL] = sp->volume;
        c->last_volume = sp->volume;
        base[AUDIO_OFF_PAN] = sp->pan;
        c->last_pan = sp->pan;

        c->finetune_lo = sp->finetune;
        c->finetune_hi = ((signed char)sp->finetune < 0) ? 0xFF : 0x00;

        c->portamento = sp->portamento;

        c->drum_flag = (unsigned char)(sp->flags & 0x80);
        c->note_pitch = sp->note_pitch;

        tempy = c->drum_flag
                    ? sp->note_pitch                                            // no transpose for drum sounds
                    : (unsigned char)(sp->note_pitch + (unsigned char)hb_state.transpose_now);

        {
            unsigned char t = (unsigned char)(note + tempy);
            if ((signed char)t < 0)
                t = 0;
            linear = ((unsigned)t) << 6;
        }
        c->freq_lo = (unsigned char)linear;
        c->freq_hi = (unsigned char)(linear >> 8);

        loop_mode = (unsigned char)(sp->flags & 0x03);
        c->loop_mode = loop_mode;

        // Sample REU address = 0x01000000 | (reu_bank << 16) -- traced and
        // confirmed exact against the reference during the port-plan design
        // pass (every sample begins at a 64KB-aligned REU offset).
        sample_addr = 0x01000000UL | ((unsigned long)sp->reu_bank << 16);
        length = (unsigned long)sp->length[0] |
                 ((unsigned long)sp->length[1] << 8) |
                 ((unsigned long)sp->length[2] << 16);

        if (loop_mode == 3)
        {
            // Loop mode 3: play only the loop A..B region, once, cropped
            // (no actual looping) -- port of .looprangeoneshot.
            unsigned long loop_a = (unsigned long)sp->loop_a[0] |
                                    ((unsigned long)sp->loop_a[1] << 8) |
                                    ((unsigned long)sp->loop_a[2] << 16);
            unsigned long loop_b = (unsigned long)sp->loop_b[0] |
                                    ((unsigned long)sp->loop_b[1] << 8) |
                                    ((unsigned long)sp->loop_b[2] << 16);
            if (loop_b == 0)
            {
                c->shadow_gate = 0x10; // don't play entire REU (matches source)
                rate = hb_get_ultimate_freq((unsigned char)(linear >> 8), (unsigned char)linear);
                audio_channel_set_rate(ua_idx, rate);
                return;
            }
            sample_addr += loop_a;
            length = loop_b - loop_a;
            loop_mode = 0; // falls through to the "no loop" cropped-playback path
        }

        base[AUDIO_OFF_SMS + 0] = (unsigned char)(sample_addr >> 24);
        base[AUDIO_OFF_SMS + 1] = (unsigned char)(sample_addr >> 16);
        base[AUDIO_OFF_SMS + 2] = (unsigned char)(sample_addr >> 8);
        base[AUDIO_OFF_SMS + 3] = (unsigned char)(sample_addr);
        base[AUDIO_OFF_SML + 0] = (unsigned char)(length >> 16);
        base[AUDIO_OFF_SML + 1] = (unsigned char)(length >> 8);
        base[AUDIO_OFF_SML + 2] = (unsigned char)(length);

        if (loop_mode == 0)
        {
            base[AUDIO_OFF_RPA + 0] = 0; base[AUDIO_OFF_RPA + 1] = 0; base[AUDIO_OFF_RPA + 2] = 0;
            base[AUDIO_OFF_RPB + 0] = 0; base[AUDIO_OFF_RPB + 1] = 0; base[AUDIO_OFF_RPB + 2] = 0;
            c->shadow_gate = 0x11; // gate only
        }
        else
        {
            unsigned long loop_a = (unsigned long)sp->loop_a[0] |
                                    ((unsigned long)sp->loop_a[1] << 8) |
                                    ((unsigned long)sp->loop_a[2] << 16);
            unsigned long loop_b = (unsigned long)sp->loop_b[0] |
                                    ((unsigned long)sp->loop_b[1] << 8) |
                                    ((unsigned long)sp->loop_b[2] << 16);
            base[AUDIO_OFF_RPA + 0] = (unsigned char)(loop_a >> 16);
            base[AUDIO_OFF_RPA + 1] = (unsigned char)(loop_a >> 8);
            base[AUDIO_OFF_RPA + 2] = (unsigned char)(loop_a);
            base[AUDIO_OFF_RPB + 0] = (unsigned char)(loop_b >> 16);
            base[AUDIO_OFF_RPB + 1] = (unsigned char)(loop_b >> 8);
            base[AUDIO_OFF_RPB + 2] = (unsigned char)(loop_b);
            c->shadow_gate = 0x13; // gate and loop
        }

        rate = hb_get_ultimate_freq((unsigned char)(linear >> 8), (unsigned char)linear);
        audio_channel_set_rate(ua_idx, rate);
    }
}

// ---------------------------------------------------------------
// hb_stop_sample_note — port of StopSampleNote.
// ---------------------------------------------------------------
static void hb_stop_sample_note(unsigned char ua_idx)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    if (c->shadow_gate == 0x13 && c->loop_mode == 2)
        c->shadow_gate = 0x11; // release (loop-with-release mode)
    else
        c->shadow_gate = 0x10; // gate off
}

// ---------------------------------------------------------------
// hb_play_pattern_row_ua — UA portion of PlayPatternRow. Per-channel
// command bytes (bit7 set, UATrackCMD) are Phase 8 scope -- skipped here.
// See hb_play_pattern_row_sid()'s comment for why last_ua_mutes isn't
// updated here either.
// ---------------------------------------------------------------
static void hb_play_pattern_row_ua(void)
{
    unsigned char ua_idx;

    for (ua_idx = 0; ua_idx < HB_UA_CHANNELS; ua_idx++)
    {
        unsigned char bit = (unsigned char)(1 << ua_idx);

        if (!(hb_songdata.sequencer_ultmutes[hb_state.seq_step] & bit))
        {
            if (hb_state.last_ua_mutes & bit) // was NOT muted last step -> just transitioned
                hb_ua[ua_idx].shadow_gate = 0x10;
            continue;
        }

        {
            unsigned char note = hb_row_buf[ua_idx * 2];
            if (note == 0)
                continue;
            if (note & 0x80)
                continue; // track command (UATrackCMD) -- Phase 8 scope, no-op for now
            if (note == 1)
            {
                hb_stop_sample_note(ua_idx);
                continue;
            }
            hb_play_sample_note(ua_idx, note);
        }
    }
}

// ---------------------------------------------------------------
// hb_register_update_ua — UA half of RegisterUpdate: flush the deferred
// shadow_gate control byte to hardware for all 7 UA channels.
// ---------------------------------------------------------------
static void hb_register_update_ua(void)
{
    unsigned char ua_idx;
    for (ua_idx = 0; ua_idx < HB_UA_CHANNELS; ua_idx++)
    {
        volatile unsigned char *base =
            (volatile unsigned char *)(unsigned long)audio_ch_base[ua_idx];
        base[AUDIO_OFF_CTR] = hb_ua[ua_idx].shadow_gate;
    }
}

// ---------------------------------------------------------------
// hb_play_pattern_row / hb_register_update — port of PlayPatternRow's and
// RegisterUpdate's overall structure (Cmd-channel command dispatch, the
// first thing in the original PlayPatternRow, is Phase 8 scope and
// skipped here). These combine the SID and UA halves in the same order
// as the original and do the once-per-row mute-state/tick-reset update
// only after BOTH halves have run.
// ---------------------------------------------------------------
static void hb_play_pattern_row(void)
{
    hb_play_pattern_row_sid();
    hb_play_pattern_row_ua();

    hb_state.last_ua_mutes = hb_songdata.sequencer_ultmutes[hb_state.seq_step];
    hb_state.last_sid_mutes = hb_songdata.sequencer_sidmutes[hb_state.seq_step];

    hb_state.tick = hb_state.tempo_ticks;
}

static void hb_register_update(void)
{
    hb_register_update_sid();
    hb_register_update_ua();
}
