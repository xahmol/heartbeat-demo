/*****************************************************************
Heartbeat Soundtracker player — C/Oscar64 port, implementation
See hbplayer.h for API documentation and NOTICE.md for attribution.

Full player: data structures, song loader, NTSC detection, init/tempo/
stop-all, tick IRQ, pattern-row fetch, SID + UA note trigger with register
flush, Cmd8x_* track-command dispatch, full per-tick modulation (vibrato/
PWM/filter-sweep/wave-arp-table-stepping/portamento -- see
hb_modulations()), and PlayFX/StopFX for one-off sample triggering.
hb_play_sid_note() sets a note's INITIAL pitch/waveform directly at trigger
time (matching PlaySIDNote); everything it sets is then continuously
modulated every tick afterwards by hb_modulate_channel(), same as the
original. See ARCHITECTURE.md for the full tick/IRQ architecture.
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

// NTSC BPM timer-lo delta, applied additively at hb_set_tempo() time instead
// of pre-baking a second full BPM table (this table is 256 bytes; a whole
// second BPM table would be 768).
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
// the PAL table as the original assembly does).
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

hb_vis_event_t    hb_vis_events[HB_VIS_MAX_EVENTS];
unsigned char     hb_vis_event_count;

// =================================================================
// Visualizer hooks -- port of player.s's "visualizerout" block
// (VisualizerFrameInit/SIDNote/SIDVelocity/SIDHalfVelocity/SampleNote/
// SampleVelocity/Reset, player.s lines ~2500-2730). See hbplayer.h's
// comment on hb_vis_events for the queue's semantics and channel
// numbering.
// =================================================================

// hb_vis_reset — see hbplayer.h for the full Input/Output/Syntax.
void hb_vis_reset(void)
{
    unsigned char i;
    for (i = 0; i < HB_VIS_MAX_EVENTS; i++)
    {
        hb_vis_events[i].note = 0;
        hb_vis_events[i].sound = 0;
        hb_vis_events[i].velocity = 0;
        hb_vis_events[i].channel = 0;
    }
    hb_vis_event_count = 0;
}

// ---------------------------------------------------------------
// hb_vis_adsr_weight — port of ADSRWeight: a perceptual "how loud does
// this envelope sound" heuristic from the attack/decay/sustain/release
// nibbles, clamped to 0-63. Verified to never overflow 8 bits at any
// intermediate step (max possible value before the final clamp is 165),
// so this is safe as plain unsigned char arithmetic with no wraparound
// concerns, unlike some of this port's other ported arithmetic.
// Input:  sr — envelope sustain/release byte (sustain in high nibble,
//              release in low nibble)
//         ad — envelope attack/decay byte (attack in high nibble, decay
//              in low nibble)
// Output: perceptual loudness weight, clamped to 0-63
// Syntax: unsigned char w = hb_vis_adsr_weight(c->sid_env_sr, c->sid_env_ad);
// ---------------------------------------------------------------
static unsigned char hb_vis_adsr_weight(unsigned char sr, unsigned char ad)
{
    unsigned char att = (unsigned char)(ad >> 4);
    unsigned char dec = (unsigned char)(ad & 0x0F);
    unsigned char sus = (unsigned char)(sr >> 4);
    unsigned char rel = (unsigned char)(sr & 0x0F);
    unsigned char bigger = (dec >= sus) ? dec : sus;
    unsigned char weight = (unsigned char)(((unsigned char)(bigger * 2 + att + dec + sus) * 2) + rel);

    weight = (unsigned char)(weight >> 1);
    return (weight < 0x3F) ? weight : 0x3F;
}

// ---------------------------------------------------------------
// hb_vis_sid_note — port of VisualizerSIDNote. Records note/sound/
// channel into the CURRENT (not-yet-committed) event slot; the slot is
// only advanced by hb_vis_sid_velocity()/hb_vis_sid_half_velocity().
// Called unconditionally at the top of hb_play_sid_note(), before the
// tied-note check (matches the original calling this before its own
// tied-note branch).
// Input:  note    — note pitch being triggered
//         sid_idx — SID chip index, 0-7
//         ch_idx  — channel within that chip, 0-2
//         sound   — instrument/sample number
// Output: none (writes the CURRENT, not-yet-committed hb_vis_events[] slot)
// Syntax: hb_vis_sid_note(note, sid_idx, ch_idx, sample_num);
// ---------------------------------------------------------------
static void hb_vis_sid_note(unsigned char note, unsigned char sid_idx, unsigned char ch_idx, unsigned char sound)
{
    unsigned char i = hb_vis_event_count;
    if (i >= HB_VIS_MAX_EVENTS)
        return;
    hb_vis_events[i].note = note;
    hb_vis_events[i].sound = sound;
    hb_vis_events[i].channel = (unsigned char)(7 + sid_idx * 3 + ch_idx);
}

// Port of VisualizerSIDVelocity (full velocity, new note) and
// VisualizerSIDHalfVelocity (half velocity, tied note) -- `half` selects
// between them. Commits the current event slot and advances the write
// index (wrapping at HB_VIS_MAX_EVENTS, matching the original).
// Input:  sr   — envelope sustain/release byte
//         ad   — envelope attack/decay byte
//         half — nonzero to halve the computed velocity (tied-note case)
// Output: none (commits and advances hb_vis_event_count)
// Syntax: hb_vis_sid_commit(c->sid_env_sr, c->sid_env_ad, 0); // real note
static void hb_vis_sid_commit(unsigned char sr, unsigned char ad, char half)
{
    unsigned char weight = hb_vis_adsr_weight(sr, ad);
    unsigned char i = hb_vis_event_count;

    if (half)
        weight = (unsigned char)(weight >> 1);

    if (i < HB_VIS_MAX_EVENTS)
    {
        hb_vis_events[i].velocity = weight;
        i++;
        if (i >= HB_VIS_MAX_EVENTS)
            i = 0;
        hb_vis_event_count = i;
    }
}

// ---------------------------------------------------------------
// hb_vis_sample_note — port of VisualizerSampleNote. Called
// unconditionally at the top of hb_play_sample_note() (before the +9
// note adjustment and before the tied-note check -- matches the
// original), NOT from hb_trigger_sample()/hb_play_fx() (the original's
// PlayFX jumps past this call site directly into ".editorentry").
//
// The "reverse UA sound numbers" step is ported as the original's exact
// byte operations (sub 1 / xor $3F / add 1, each wrapping like the 6502
// SBC/EOR/ADC), not simplified to "65 - sound" -- that simplification
// only coincidentally matches for sound_in 1-64; it diverges for
// sound_in==0 (the tied-note case), where the real 6502 sequence
// produces $C1 via 8-bit wraparound, not a small "clean" number.
// Unexplained in the source beyond its own comment -- likely just a
// display/palette convention of the original author's own visualizer.
// Input:  note   — note pitch being triggered
//         ua_idx — Ultimate Audio channel, 0-6
//         sound  — sample number (1-64), reversed before storing (see above)
// Output: none (writes the CURRENT, not-yet-committed hb_vis_events[] slot)
// Syntax: hb_vis_sample_note(raw_note, ua_idx, sample_num);
// ---------------------------------------------------------------
static void hb_vis_sample_note(unsigned char note, unsigned char ua_idx, unsigned char sound)
{
    unsigned char i = hb_vis_event_count;
    unsigned char s;
    if (i >= HB_VIS_MAX_EVENTS)
        return;

    s = (unsigned char)(sound - 1);
    s = (unsigned char)(s ^ 0x3F);
    s = (unsigned char)(s + 1);

    hb_vis_events[i].note = note;
    hb_vis_events[i].sound = s;
    hb_vis_events[i].channel = ua_idx;
}

// Port of VisualizerSampleVelocity: commits the current event slot's
// velocity and advances the write index (wrapping), same as
// hb_vis_sid_commit()'s SID counterpart. Called from hb_play_sample_note()
// (tied-note branch, half of last_volume) and hb_trigger_sample() (real
// note, sample's own volume) -- both reachable from hb_play_fx() too,
// since hb_trigger_sample() is PlayFX's shared continuation.
// Input:  volume — velocity/volume value to store (0-63 range expected)
// Output: none (commits and advances hb_vis_event_count)
// Syntax: hb_vis_sample_velocity(sp->volume);
static void hb_vis_sample_velocity(unsigned char volume)
{
    unsigned char i = hb_vis_event_count;
    if (i < HB_VIS_MAX_EVENTS)
    {
        hb_vis_events[i].velocity = volume;
        i++;
        if (i >= HB_VIS_MAX_EVENTS)
            i = 0;
        hb_vis_event_count = i;
    }
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
// See hbplayer.h for the full Input/Output/Syntax.
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
// the frequency lookup (hb_get_sid_freq()) select between the PAL/NTSC
// embedded tables via this flag at each use instead, so no runtime
// mutation of #embed-sourced data is needed.
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
// See hbplayer.h for the full Input/Output/Syntax.
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
// deliberately skipping the UA reset -- this is the reference player's
// actual behavior, ported as-is for exactness).
// ---------------------------------------------------------------
// hb_init_sid_image_and_volumes — port of InitSIDImageAndVolumes: zeroes
// all SID working state, then re-reads each chip's address/volume from
// the currently-loaded hb_songdata.
// Input:  none (reads hb_songdata.sid_addresses[]/sid_volumes[])
// Output: none (resets hb_sids[])
// Syntax: hb_init_sid_image_and_volumes(); // called by hb_stop_all()
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

// hb_init_ua_and_sids — port of InitUltimateAudioAndSIDs: zeroes all UA
// channel working state, then falls through into
// hb_init_sid_image_and_volumes() to reset SID state too (unlike
// hb_stop_all(), which calls only the SID half).
// Input:  none
// Output: none (resets hb_ua[] and hb_sids[])
// Syntax: hb_init_ua_and_sids(); // called by hb_init()
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
// table mutation.
//
// php/plp (not sei/cli) to match the original exactly: this may be called
// from within the tick IRQ via a Bt track command, where unconditionally
// re-enabling interrupts at the end (cli) would be wrong.
// See hbplayer.h for the full Input/Output/Syntax.
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
// See hbplayer.h for the full Input/Output/Syntax.
// ---------------------------------------------------------------
void hb_stop_all(void)
{
    hb_state.play_mode = 0;
    hb_init_sid_image_and_volumes();
}

// Forward declarations -- implementations are below hb_fetch_pattern_row()
// (SID/UA note trigger + shadow flush). Kept as static internals, not part
// of the public API in hbplayer.h.
static void hb_sid_hard_restart(void);
static void hb_early_gate_on(void);
static void hb_trigger_sample(unsigned char ua_idx, unsigned char note, unsigned char sample_idx);
static void hb_play_pattern_row(void);
static void hb_register_update(void);
static void hb_modulations(void);
static void hb_ua_track_cmd(unsigned char ua_idx, unsigned char cmd_byte);
static void hb_sid_track_cmd(unsigned char sid_idx, unsigned char ch_idx, unsigned char cmd_byte);
static void hb_cmd_channel_cmd(unsigned char cmd_byte);

// ---------------------------------------------------------------
// hb_tick — port of PlayerUpdate (SID + UA note trigger, Cmd8x_* track
// commands, and full per-tick Modulations -- all phases complete).
//
// Mirrors the original's MusicPlayMode dispatch exactly:
//   bit7 set ($80)      -> return immediately, skip even Modulations/
//                          RegisterUpdate (matches .alloff)
//   ==0 (idle/ended)    -> skip tick/row logic, but STILL run Modulations
//                          and flush registers (matches .done -- envelope
//                          decay, vibrato, filter sweep etc. must keep
//                          running even after notes stop)
//   otherwise (playing) -> tick countdown + row-fetch/hard-restart/
//                          early-gate-on dispatch, then Modulations +
//                          register flush
//
// __interrupt: Oscar64 auto-saves whatever ZP it statically determines
// this function's call tree touches (see hb_irq's comment for the full
// gap-analysis method) -- do NOT assume past results still hold if this
// body grows: re-verify via -g build + .asm whenever a change adds a new
// call into this function's tree (PlayFX/StopFX are NOT called from it,
// so shouldn't affect this -- but re-check anyway).
// Input:  none (reads/advances hb_state; not part of the public API --
//         called only from hb_irq's raw __asm dispatch, never directly)
// Output: none
// Syntax: not called directly -- fires ~195 Hz via CIA1 Timer A once
//         hb_init() has installed hb_irq at $0314/$0315
// ---------------------------------------------------------------
__interrupt void hb_tick(void)
{
    // DELIBERATE DEVIATION from the original: player.s's VisualizerFrameInit
    // resets the event queue's write index at the start of every TICK
    // (~195 Hz), unconditionally. That's correct for the original's own
    // visualizer, which is assumed to consume the queue synchronously
    // within the same tick (or an equally tightly-coupled context). This
    // port's visualizer (src/visualizer.c) instead consumes it once per
    // VIC frame (~50 Hz, via vic_waitFrame()) from the main loop -- roughly
    // once every 4 ticks. Resetting every tick as the original does would
    // wipe out almost every note-on event before the main loop ever gets a
    // chance to see it (notes only trigger roughly once per ROW, i.e. once
    // every ~20-25 ticks, so the overwhelming majority of individual ticks
    // have nothing to report -- exactly the tick a slow poll is likely to
    // land on). So hb_tick does NOT reset the queue at all: events
    // accumulate (up to HB_VIS_MAX_EVENTS, then wrap) until whoever is
    // reading it resets hb_vis_event_count themselves once they're done
    // with a batch -- see hb_vis_reset() / src/visualizer.c.
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
        else if (hb_state.tick == hb_songdata.hardrestart_gateon_time)
        {
            hb_early_gate_on();
        }
    }

    hb_modulations();
    hb_register_update();
}

// ---------------------------------------------------------------
// hb_irq — port of PlayerIRQ. Raw __asm entry point (no C prologue),
// installed at $0314/$0315 by hb_init(). Checks $D019 bit0 first to tell a
// VIC raster IRQ apart from the CIA1 Timer A IRQ that actually drives the
// player tick.
//
// Two IRQ sources are active at once: a fixed-line raster IRQ (independent
// of tempo), and CIA1 Timer A (tempo-dependent tick rate, ~195Hz) which
// runs the real player update. The two branches end differently, by design:
//
//   - Raster branch: acks the raster IRQ, then chains to KERNAL $EA31,
//     whose own sequence includes SCNKEY -- this is the only place
//     keyboard scanning happens, at the raster IRQ's fixed ~50/60Hz rate,
//     independent of however slow the current BPM's tick rate is.
//   - CIA1 (tick) branch: acks the CIA1 IRQ, then bypasses the KERNAL IRQ
//     tail entirely -- restores A/X/Y itself, in the exact order the
//     KERNAL's own hardware IRQ entry ($FF48) pushed them (Y last, so
//     PLA/TAY pops it first, then PLA/TAX, then a final PLA restores A),
//     and RTIs directly. This branch never touches SCNKEY or KERNAL's
//     jiffy-clock/STOP-key logic -- it fires at the ~195Hz tick rate, far
//     too often for either.
//
// Per the "named asm blocks are addresses, not callables" rule (see
// oscar64manual.md), this is installed via `*((void**)0x0314) = hb_irq;`,
// never called with hb_irq().
//
// ZERO-PAGE GAP: hb_tick's call tree includes hb_play_sid_note(), whose
// `note << 6` frequency-scaling compiles to a call to Oscar64's mul16by8
// runtime routine (rather than unrolled shifts). Verified via -g build +
// .asm inspection that mul16by8 uses zero-page $02 as scratch, in addition
// to ACCU+0/1 ($1B/$1C) -- and that $02 is NOT among the ZP locations
// Oscar64's __interrupt prologue for hb_tick auto-saves (currently saves
// WORK+0..3, P0-P10, ACCU+0..3, T0-T3, and $4D-$51 -- but never $02).
//
// The only other runtime helper reachable from hb_tick's call tree is
// divmod (WORK+0..3/ACCU+0..1, already auto-saved) -- a direct scan of
// every reachable function's own disassembly for unnamed/raw zero-page
// addressing (as opposed to the named ACCU/WORK/P/T registers) finds
// nothing else; they only ever touch zero page via those already-covered
// registers. The auto-save prologue itself grows as the call tree grows
// -- Oscar64 finds more, never less -- so this needs re-verifying (see
// ARCHITECTURE.md's zero-page gap analysis methodology) whenever
// hb_tick's call tree changes, not just once.
//
// Manually saved/restored here around the hb_tick call, following the
// same method as modplay_irq's documented gap analysis in
// UltimateDemo2026/include/modplay.c.
// Input:  none (hardware-invoked IRQ handler, not called directly)
// Output: none
// Syntax: *((void **)0x0314) = hb_irq; // install once, in hb_init() -- a
//         named __asm block is an ADDRESS, never call it as hb_irq()
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

    // Bypass the KERNAL IRQ tail entirely: restore A/X/Y in the exact
    // order the KERNAL's own hardware IRQ entry ($FF48) pushed them
    // (A, then X, then Y -- so Y is on top of the stack here) and RTI
    // directly -- see this function's own header comment above.
    pla
    tay
    pla
    tax
    pla
    rti

hb_irq_raster:
    sta $d019           // acknowledge raster IRQ
    jmp $ea31           // $EA31's own sequence includes SCNKEY
}

// ---------------------------------------------------------------
// hb_init — port of PlayerInit. Installs the tick IRQ and resets player
// state. Call hb_detect_ntsc() before this.
// See hbplayer.h for the full Input/Output/Syntax.
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
// hb_fetch_pattern_row — port of FetchPatternRow: the REU row fetch and
// pattern-pointer advance. Does NOT include SIDHardRestart (that's
// hb_sid_hard_restart(), called separately by hb_tick's dispatch ahead of
// a hard-restart, matching the original's own split).
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
// MusicPlayMode==2 (pattern-loop/editor "PatternBuffer" mode) is not
// implemented -- the standalone player itself never uses it either. The
// fallback below just loops the current pattern from row 0 instead of
// reading the (unimplemented) PatternBuffer at $C000.
// See hbplayer.h for the full Input/Output/Syntax.
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
// SID note trigger + shadow flush
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
// Input:  freq_hi, freq_lo — 16-bit linear frequency ("note << 6" internal
//                            fixed-point unit), split into high/low bytes
// Output: 16-bit SID frequency register value ($D400/$D407/$D40E etc.)
// Syntax: unsigned reg = hb_get_sid_freq(c->base_freq_hi, c->base_freq_lo);
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
// Input:  sid_idx — SID chip index, 0-7
// Output: 1 if the chip is muted this step, 0 if not (also force-releases
//         the chip's 3 channel gates on the not-muted-to-muted transition)
// Syntax: if (hb_check_sid_mute(sid_idx)) continue;
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
// hb_sid_hard_restart — port of SIDHardRestart (split out from
// hb_fetch_pattern_row()'s port of FetchPatternRow, since it's SID-specific
// register-image work, not row-fetch/sequencer work). For each unmuted
// SID channel with a genuine new note incoming this row (not empty, not
// an effect/command byte, not a stop, not a tied note), forces the
// envelope/waveform/frequency to a clean silent state ahead of the real
// note-on, avoiding audible artifacts from whatever the previous note
// left behind.
// Input:  none (reads hb_row_buf[]/hb_songdata, updates hb_sids[])
// Output: none
// Syntax: hb_sid_hard_restart(); // called by hb_tick() at tick==hardrestart_time
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
// hb_early_gate_on — port of EarlyGateOn, called at
// tick==hardrestart_gateon_time ("last tick 1-2 of hard restart"), a beat
// after hb_sid_hard_restart's silencing -- pre-arms the envelope and
// key-up mask ahead of the real note trigger so the gate's low->high
// transition on the actual note-on doesn't click. Uses the exact same
// note-validity checks as hb_sid_hard_restart (same row-buffer layout),
// but reads the CURRENT row's about-to-play instrument's ADSR directly
// (not a fixed silence template) and sets a fixed waveform byte from the
// song data (hardrestart_gateon_wave) rather than clearing it.
// Input:  none (reads hb_row_buf[]/hb_songdata, updates hb_sids[])
// Output: none
// Syntax: hb_early_gate_on(); // called by hb_tick() at tick==hardrestart_gateon_time
// ---------------------------------------------------------------
static void hb_early_gate_on(void)
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
            unsigned char sample_num = hb_row_buf[off + 1];
            hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];

            if (note == 0)       continue; // no note
            if (note & 0x80)      continue; // effect/command, not a note
            if (note == 1)        continue; // stop note
            if (sample_num == 0)  continue; // tied note, no early gate-on

            {
                hb_inst_params_t *inst = &hb_songdata.inst_params[sample_num - 1];
                c->sid_env_ad = inst->env_ad;
                c->sid_env_sr = inst->env_sr;
            }

            c->sid_wave = hb_songdata.hardrestart_gateon_wave;
            c->gate_mask = 0xFF; // gate up in key-up mask
        }
    }
}

// Filter-select bit masks for SIDFiltCtrl's low nibble (bit0/1/2 = enable
// filter on SID channel 0/1/2). Ported literally from ChannelAND/ChannelORA
// (player.s ~2759-2769), indexed directly by ch_idx (0-2) here since this
// port already has a real channel index -- the original's contrived
// "X&0x1F, lsr, lsr -> table index 0,1,3" exists only because X there is a
// 32-byte-strided chip/channel parameter offset, not a plain 0-2 index.
// NOTE: the AND (disable) table is intentionally asymmetric in the
// original -- channel 2's entry (0xF9) clears BOTH bits 1 and 2, not just
// bit 2, unlike the ORA (enable) table's clean single-bit-per-channel
// mapping. Ported as-is, not "fixed", per this project's bit-exactness goal.
static const unsigned char hb_channel_and[3] = { 0xFE, 0xFD, 0xF9 };
static const unsigned char hb_channel_ora[3] = { 0x01, 0x02, 0x04 };

// ---------------------------------------------------------------
// hb_play_sid_note — full port of PlaySIDNote.
//
// Sets up envelope, gate, finetune, portamento speed/target-reached,
// PWM/vibrato/filter parameters, and wave/arp-table-stepping state
// (step=0xFF/count=1 so hb_modulate_channel reads real step 0 on the very
// next Modulations pass -- which happens later in this SAME tick, since
// hb_tick calls hb_play_pattern_row() before hb_modulations()). Does NOT
// itself compute SIDFreq or the initial waveform -- matches the original,
// which leaves both to ModulateChannel's first pass after trigger, not
// PlaySIDNote itself.
// Input:  sid_idx — SID chip index, 0-7
//         ch_idx  — channel within that chip, 0-2
//         note    — note pitch to trigger (raw pattern-row note byte)
// Output: none (updates hb_sids[sid_idx].ch[ch_idx] working state)
// Syntax: hb_play_sid_note(sid_idx, ch_idx, transposed_note);
// ---------------------------------------------------------------
static void hb_play_sid_note(unsigned char sid_idx, unsigned char ch_idx, unsigned char note)
{
    hb_sid_chip_t *chip = &hb_sids[sid_idx];
    hb_sid_channel_t *c = &chip->ch[ch_idx];
    unsigned char off = (unsigned char)(14 + sid_idx * 6 + ch_idx * 2);
    unsigned char sample_num = hb_row_buf[off + 1];
    unsigned linear = ((unsigned)note) << 6; // BaseFreq/H = note*64 (two lsr/ror pairs in the original)

    hb_vis_sid_note(note, sid_idx, ch_idx, sample_num); // unconditional, before the tied-note check

    if (sample_num == 0)
    {
        // Tied note: only retarget portamento (or jump instantly if speed is 0).
        hb_vis_sid_commit(c->sid_env_sr, c->sid_env_ad, 1); // half velocity (still-active envelope)

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

        c->instr_lo = (unsigned char)(sample_num - 1); // index, not a real pointer (see hbplayer.h)
        c->instr_hi = 1;                                // nonzero = instrument active

        c->sid_env_ad = inst->env_ad;
        c->sid_env_sr = inst->env_sr;

        hb_vis_sid_commit(c->sid_env_sr, c->sid_env_ad, 0); // full velocity (new envelope)

        c->finetune = inst->finetune;
        c->finetune_hi = (ft < 0) ? 0xFF : 0x00; // sign-extend

        c->portamento = inst->portamento;

        // ---- PWM start/rate/top/bottom ($14-$16) ----
        // pwm_start==0 means "don't reset the pulse-width register" --
        // keep whichever direction/value PWM is already sweeping in
        // (free-running), only updating the rate; if the current sweep
        // direction is downward (pwm_rate_hi != 0), the new rate from this
        // instrument is negated to match that direction instead of
        // resetting it upward.
        if (inst->pwm_start != 0)
        {
            unsigned pw16 = 0x8000u | ((unsigned)inst->pwm_start << 4);
            c->sid_pw_lo = (unsigned char)pw16;
            c->sid_pw_hi = (unsigned char)(pw16 >> 8);
            c->pwm_rate = inst->pwm_rate;
            c->pwm_rate_hi = 0x00;
        }
        else if (c->pwm_rate_hi == 0)
        {
            c->pwm_rate = inst->pwm_rate;
            c->pwm_rate_hi = 0x00;
        }
        else
        {
            c->pwm_rate = (unsigned char)(0 - inst->pwm_rate);
            c->pwm_rate_hi = 0xFF;
        }
        c->pwm_bottom_hi = (unsigned char)((inst->pwm_topbottom & 0x0F) | 0x80);
        c->pwm_top_hi    = (unsigned char)((inst->pwm_topbottom >> 4) | 0x80);

        // ---- Vibrato delay/width/rate ($17-$19) ----
        c->vib_delay = inst->vib_delay;
        c->vib_width = (unsigned char)(inst->vib_width >> 1);
        {
            unsigned vibrate16 = (unsigned)inst->vib_rate << 4;
            c->vib_rate    = (unsigned char)vibrate16;
            c->vib_rate_hi = (unsigned char)(vibrate16 >> 8);
        }

        // ---- Filter type/resonance/cutoff ($1A-$1F) -- chip-level ----
        if (inst->filter_type == 0)
        {
            // No filter for THIS channel: just clear its enable bit(s),
            // preserving the chip's resonance nibble and other channels'
            // bits. cutoff_mod/top/bottom are deliberately left untouched
            // -- they're chip-level and may still be driven by another
            // channel on the same chip that DOES want filtering.
            chip->filt_ctrl = (unsigned char)(chip->filt_ctrl & hb_channel_and[ch_idx]);
        }
        else
        {
            chip->volume = (unsigned char)((chip->volume & 0x0F) | ((inst->filter_type & 0x0F) << 4));
            chip->cutoff_bounce = (inst->filter_type & 0x10) ? 0x80 : 0x00;

            chip->filt_ctrl = (unsigned char)(((chip->filt_ctrl | hb_channel_ora[ch_idx]) & 0x0F)
                                               | ((inst->filter_resonance & 0x0F) << 4));

            if (inst->cutoff_init != 0)
            {
                unsigned cutoff16 = 0x8000u | ((unsigned)inst->cutoff_init << 5);
                chip->filter_lo = (unsigned char)cutoff16;
                chip->filter_hi = (unsigned char)(cutoff16 >> 8);
                chip->cutoff_mod = inst->cutoff_mod;
            }
            else
            {
                // No cutoff init: keep the CURRENT direction -- if this
                // instrument's mod rate would reverse it, invert the rate
                // instead of the direction (matches the original's
                // "invertmod"/"normalmod" branch pair exactly).
                signed char new_mod = (signed char)inst->cutoff_mod;
                signed char cur_mod = (signed char)chip->cutoff_mod;
                char same_dir = (new_mod < 0) ? (cur_mod < 0) : (cur_mod >= 0);
                chip->cutoff_mod = same_dir ? inst->cutoff_mod : (unsigned char)(0 - inst->cutoff_mod);
            }

            {
                unsigned top16 = 0x8000u | ((unsigned)inst->cutoff_top << 5);
                chip->cutoff_top_lo = (unsigned char)top16;
                chip->cutoff_top_hi = (unsigned char)(top16 >> 8);
                unsigned bottom16 = 0x8000u | ((unsigned)inst->cutoff_bottom << 5);
                chip->cutoff_bottom_lo = (unsigned char)bottom16;
                chip->cutoff_bottom_hi = (unsigned char)(bottom16 >> 8);
            }
        }

        c->vib_phase = 0;
        c->vib_frac = 0;
        c->target_freq_hi = 0xFF; // portamento target reached (nothing pending)

        c->wave_arp_speed = 4;
        c->wave_arp_count = 1;
        c->wave_arp_step = 0xFF; // wraps to step 0 on the first increment (in hb_modulate_channel)
        c->gate_mask = 0xFF;     // gate from wave table

        c->base_freq_lo = (unsigned char)linear;
        c->base_freq_hi = (unsigned char)(linear >> 8);
    }
}

// ---------------------------------------------------------------
// hb_play_pattern_row_sid — SID portion of PlayPatternRow. The Cmd-channel
// command check (checked first in the original) and per-channel command
// bytes (bit7 set) are handled by hb_cmd_channel_cmd()/hb_sid_track_cmd()
// (see the Cmd8x_* track-command dispatch section below) -- skipped here.
//
// Does NOT update last_ua_mutes/last_sid_mutes or reset hb_state.tick --
// those happen once, in hb_play_pattern_row() below, AFTER both this and
// the UA dispatch (hb_play_pattern_row_ua()) have run. Doing it here
// instead would make the UA loop's mute-transition check always compare
// the current row's mutes against itself instead of the previous row's.
// Input:  none (reads hb_row_buf[]/hb_state, updates hb_sids[])
// Output: none
// Syntax: hb_play_pattern_row_sid(); // called by hb_play_pattern_row()
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
            {
                hb_sid_track_cmd(sid_idx, ch_idx, note);
                continue;
            }

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
// result.
// Input:  chip — pointer to the SID chip whose register image to flush
// Output: none (writes directly to the chip's real hardware registers)
// Syntax: hb_write_one_sid(&hb_sids[i]);
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

// hb_register_update_sid — flushes every populated SID chip's register
// image to hardware via hb_write_one_sid().
// Input:  none
// Output: none
// Syntax: hb_register_update_sid(); // called by hb_register_update()
static void hb_register_update_sid(void)
{
    unsigned char i;
    for (i = 0; i < HB_MAX_SIDS; i++)
        if (hb_sids[i].addr != 0)
            hb_write_one_sid(&hb_sids[i]);
}

// =================================================================
// Ultimate Audio sample trigger + shadow flush
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
// Input:  freq_hi, freq_lo — 16-bit linear frequency ("note << 6" internal
//                            fixed-point unit), split into high/low bytes
// Output: Ultimate Audio playback rate register value
// Syntax: unsigned rate = hb_get_ultimate_freq((unsigned char)(linear >> 8), (unsigned char)linear);
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
// hb_play_sample_note — port of PlaySampleNote's row-triggered entry:
// reads the sample number from the row buffer, handles the tied-note case
// (sample number 0), and otherwise hands off to hb_trigger_sample() for
// everything from there on (matches the original's ".editorentry" split,
// shared with hb_play_fx()).
//
// Unlike SID's frequency (which is only READ by hardware once
// RegisterUpdate flushes SIDImage), the UA rate register has no shadow/
// batch mechanism in the original -- only the gate/control byte is
// deferred via UAShadowGate. So hb_trigger_sample() writes sample-start/
// length/loop-point/volume/pan/rate directly to hardware immediately
// (matching the original's own direct pokes), and only defers the final
// gate-trigger byte into hb_ua[].shadow_gate for hb_register_update_ua()
// to flush.
// Input:  ua_idx   — Ultimate Audio channel, 0-6
//         raw_note — note pitch from the pattern row, before the "+9" adjust
// Output: none (updates hb_ua[ua_idx] and/or writes hardware directly)
// Syntax: hb_play_sample_note(ua_idx, note);
// ---------------------------------------------------------------
static void hb_play_sample_note(unsigned char ua_idx, unsigned char raw_note)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    unsigned char sample_num = hb_row_buf[ua_idx * 2 + 1];
    unsigned char note = (unsigned char)(raw_note + 9); // "note value adjust" (matches PlayFX's own +9)

    // Unconditional, before both the +9 adjustment's effects and the
    // tied-note check -- matches the original calling VisualizerSampleNote
    // with the raw (pre-adjustment) note at the very top of PlaySampleNote.
    // NOT called from hb_trigger_sample()/hb_play_fx() -- PlayFX jumps past
    // this call site directly into the shared continuation.
    hb_vis_sample_note(raw_note, ua_idx, sample_num);

    if (sample_num == 0)
    {
        // Tied note: retarget portamento only (or jump instantly if speed is 0).
        hb_vis_sample_velocity((unsigned char)(c->last_volume >> 1)); // half volume for tied notes

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

    hb_trigger_sample(ua_idx, note, (unsigned char)(sample_num - 1));
}

// ---------------------------------------------------------------
// hb_trigger_sample — port of PlaySampleNote's ".editorentry" continuation:
// everything from computing the sample pointer onward, shared by BOTH the
// normal row-triggered path (hb_play_sample_note, after its tied-note
// check) and hb_play_fx() (which has no row buffer / tied-note concept at
// all and jumps straight here in the original). `note` is the
// already-"+9"-adjusted note value; `sample_idx` is 0-63 (already -1'd).
// Input:  ua_idx     — Ultimate Audio channel, 0-6
//         note       — already "+9"-adjusted note pitch
//         sample_idx — sample/instrument index, 0-63 (already -1 from the
//                      1-64 song-data sample number)
// Output: none (updates hb_ua[ua_idx] and writes hardware directly)
// Syntax: hb_trigger_sample(ua_idx, note, sample_num - 1);
// ---------------------------------------------------------------
static void hb_trigger_sample(unsigned char ua_idx, unsigned char note, unsigned char sample_idx)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    volatile unsigned char *base = (volatile unsigned char *)(unsigned long)audio_ch_base[ua_idx];

    {
        hb_sample_params_t *sp = &hb_songdata.sample_params[sample_idx];
        unsigned char tempy;
        unsigned linear, rate;
        unsigned char loop_mode;
        unsigned long sample_addr, length;

        c->sample_lo = sample_idx; // index, not a real pointer
        c->sample_hi = 1;           // nonzero = sample active

        // Immediate stop -- "note: writing directly to UA register" in
        // the original; this is a real direct poke, not shadow-deferred.
        base[AUDIO_OFF_CTR] = 0x10;
        c->shadow_gate = 0x10;

        if (sp->reu_bank == 0)
            return; // no sample in this slot

        base[AUDIO_OFF_VOL] = sp->volume;
        c->last_volume = sp->volume;
        hb_vis_sample_velocity(sp->volume); // reachable from hb_play_fx() too (shared continuation)
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

        // Sample REU address = 0x01000000 | (reu_bank << 16) -- every sample
        // begins at a 64KB-aligned REU offset.
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
// Input:  ua_idx — Ultimate Audio channel, 0-6
// Output: none (defers a release/off byte into hb_ua[ua_idx].shadow_gate)
// Syntax: hb_stop_sample_note(ua_idx);
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
// command bytes (bit7 set, UATrackCMD) are handled by hb_ua_track_cmd()
// (see the Cmd8x_* track-command dispatch section below) -- skipped here.
// See hb_play_pattern_row_sid()'s comment for why last_ua_mutes isn't
// updated here either.
// Input:  none (reads hb_row_buf[]/hb_state, updates hb_ua[])
// Output: none
// Syntax: hb_play_pattern_row_ua(); // called by hb_play_pattern_row()
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
            {
                hb_ua_track_cmd(ua_idx, note);
                continue;
            }
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
// Input:  none
// Output: none
// Syntax: hb_register_update_ua(); // called by hb_register_update()
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
// RegisterUpdate's overall structure. Checks the Cmd channel first
// (RowBuffer[62]), matching the original's "Cmd channel first" comment,
// then the SID and UA halves in the same order as the original, and does
// the once-per-row mute-state/tick-reset update only after all three
// have run.
// Input:  none (reads hb_row_buf[]/hb_state)
// Output: none (resets hb_state.tick for the next row)
// Syntax: hb_play_pattern_row(); // called by hb_tick() at tick==0
// ---------------------------------------------------------------
static void hb_play_pattern_row(void)
{
    if (hb_row_buf[62] & 0x80)
        hb_cmd_channel_cmd(hb_row_buf[62]);

    hb_play_pattern_row_sid();
    hb_play_pattern_row_ua();

    hb_state.last_ua_mutes = hb_songdata.sequencer_ultmutes[hb_state.seq_step];
    hb_state.last_sid_mutes = hb_songdata.sequencer_sidmutes[hb_state.seq_step];

    hb_state.tick = hb_state.tempo_ticks;
}

// hb_register_update — flushes both SID and UA register images to
// hardware. Runs every tick regardless of play_mode (see hb_tick()).
// Input:  none
// Output: none
// Syntax: hb_register_update(); // called by hb_tick() every tick
static void hb_register_update(void)
{
    hb_register_update_sid();
    hb_register_update_ua();
}

// =================================================================
// Cmd8x_* track-command dispatch
// =================================================================
//
// Command byte $80-$8A (11 commands): Pa(pan)/Bt(tempo)/Dn(slide down)/
// Fi(finetune)/Iv(SID volume)/Le(pattern length)/Co(filter cutoff)/
// Po(portamento)/Up(slide up)/Vo(volume)/Xt(kill/ext-sync). Each of the
// three channel *types* (UA, SID, Cmd) has its own table mapping command
// -> handler, with some commands ignored (CmdNone) on some channel
// types -- ported as a switch per channel type instead of the original's
// word-table-of-function-pointers (see UACommandTable/SIDCommandTable/
// CmdCommandTable in player.s), same behavior either way.
//
// Command parameter is always RowBuffer[off+1] (the same byte slot that
// holds "sample number" for a normal note) -- read by the dispatchers
// below, not by each handler individually.

unsigned char hb_ext_out;

// ---- Cmd80 Pa: pan (UA only; ignored on SID/Cmd) ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — pan value, low nibble
// Output: none
// Syntax: hb_cmd_pa_ua(ua_idx, param);
static void hb_cmd_pa_ua(unsigned char ua_idx, unsigned char param)
{
    hb_ua[ua_idx].last_pan = (unsigned char)(param & 0x0F); // stored; applied via freq/rate path already
    // NOTE: the original writes UAPan directly as an ACTIVE (non-shadowed)
    // register, matching PlaySampleNote's own direct pan poke -- do the
    // same here for consistency (immediate, not deferred).
    {
        volatile unsigned char *base = (volatile unsigned char *)(unsigned long)audio_ch_base[ua_idx];
        base[AUDIO_OFF_PAN] = (unsigned char)(param & 0x0F);
    }
}

// ---- Cmd81 Bt: BPM tempo (all channel types) ----
// Input:  param — new tempo, encoded as BPM-64
// Output: none
// Syntax: hb_cmd_bt(param);
static void hb_cmd_bt(unsigned char param)
{
    hb_set_tempo(param);
}

// ---- Cmd82 Dn: slide down (UA/SID single-channel; Cmd = all channels) ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — portamento speed
// Output: none (retargets portamento toward frequency 0)
// Syntax: hb_cmd_dn_ua(ua_idx, param);
static void hb_cmd_dn_ua(unsigned char ua_idx, unsigned char param)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    c->portamento = param;
    c->target_freq_lo = 0;
    c->target_freq_hi = 0;
}

// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         param — portamento speed
// Output: none (retargets portamento toward frequency 0)
// Syntax: hb_cmd_dn_sid(sid_idx, ch_idx, param);
static void hb_cmd_dn_sid(unsigned char sid_idx, unsigned char ch_idx, unsigned char param)
{
    hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];
    c->portamento = param;
    c->target_freq_lo = 0;
    c->target_freq_hi = 0;
}

// Shared by Cmd82_Dn (Cmd channel) and Cmd88_Up (Cmd channel) -- slides
// ALL SID + UA channels at once, matching Cmd82_Dn_SlideDown_Cmd/
// Cmd88_Up_SlideUp_Cmd's shared ".common" tail in the original.
// Input:  param             — portamento speed
//         target_lo/target_hi — target linear frequency to slide toward
// Output: none
// Syntax: hb_cmd_slide_all(param, 0, 0); // slide down (target $0000)
static void hb_cmd_slide_all(unsigned char param, unsigned char target_lo, unsigned char target_hi)
{
    unsigned char i, ch;
    for (i = 0; i < HB_UA_CHANNELS; i++)
    {
        hb_ua[i].portamento = param;
        hb_ua[i].target_freq_lo = target_lo;
        hb_ua[i].target_freq_hi = target_hi;
    }
    for (i = 0; i < HB_MAX_SIDS; i++)
        for (ch = 0; ch < 3; ch++)
        {
            hb_sids[i].ch[ch].portamento = param;
            hb_sids[i].ch[ch].target_freq_lo = target_lo;
            hb_sids[i].ch[ch].target_freq_hi = target_hi;
        }
}

// ---- Cmd83 Fi: finetune (UA/SID; ignored on Cmd) ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — signed finetune value
// Output: none
// Syntax: hb_cmd_fi_ua(ua_idx, param);
static void hb_cmd_fi_ua(unsigned char ua_idx, unsigned char param)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    c->finetune_lo = param;
    c->finetune_hi = ((signed char)param < 0) ? 0xFF : 0x00;
}

// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         param — signed finetune value
// Output: none
// Syntax: hb_cmd_fi_sid(sid_idx, ch_idx, param);
static void hb_cmd_fi_sid(unsigned char sid_idx, unsigned char ch_idx, unsigned char param)
{
    hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];
    c->finetune = param;
    c->finetune_hi = ((signed char)param < 0) ? 0xFF : 0x00;
}

// ---- Cmd84 Iv: SID volume (current chip on SID channel; ALL chips on
// UA/Cmd channels -- low nibble only, filter type nibble preserved) ----
// Input:  sid_idx — SID chip, 0-7; param — volume, low nibble used (0-15)
// Output: none
// Syntax: hb_cmd_iv_sid(sid_idx, param);
static void hb_cmd_iv_sid(unsigned char sid_idx, unsigned char param)
{
    hb_sids[sid_idx].volume = (unsigned char)((hb_sids[sid_idx].volume & 0xF0) | (param & 0x0F));
}

// Input:  param — volume, low nibble used (0-15), applied to every SID chip
// Output: none
// Syntax: hb_cmd_iv_all(param);
static void hb_cmd_iv_all(unsigned char param)
{
    unsigned char i;
    for (i = 0; i < HB_MAX_SIDS; i++)
        hb_cmd_iv_sid(i, param);
}

// ---- Cmd85 Le: pattern length (all channel types), clamped to 64 ----
// Input:  param — new pattern length, clamped to a max of 64 (0x40)
// Output: none
// Syntax: hb_cmd_le(param);
static void hb_cmd_le(unsigned char param)
{
    hb_state.patt_length = (param < 0x40) ? param : 0x40;
}

// ---- Cmd86 Co: filter cutoff (SID only; ignored on UA/Cmd) ----
// Input:  sid_idx — SID chip, 0-7; param — cutoff value
// Output: none
// Syntax: hb_cmd_co(sid_idx, param);
static void hb_cmd_co(unsigned char sid_idx, unsigned char param)
{
    unsigned v = ((unsigned)param) << 5; // matches 3x lsr/ror -> effectively <<5 into a 16-bit split
    hb_sids[sid_idx].filter_hi = (unsigned char)((v >> 8) | 0x80);
    hb_sids[sid_idx].filter_lo = (unsigned char)v;
}

// ---- Cmd87 Po: portamento speed only, no target change (UA/SID; ignored on Cmd) ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — portamento speed
// Output: none
// Syntax: hb_cmd_po_ua(ua_idx, param);
static void hb_cmd_po_ua(unsigned char ua_idx, unsigned char param)
{
    hb_ua[ua_idx].portamento = param;
}

// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         param — portamento speed
// Output: none
// Syntax: hb_cmd_po_sid(sid_idx, ch_idx, param);
static void hb_cmd_po_sid(unsigned char sid_idx, unsigned char ch_idx, unsigned char param)
{
    hb_sids[sid_idx].ch[ch_idx].portamento = param;
}

// ---- Cmd88 Up: slide up (UA/SID single-channel; Cmd = all channels) ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — portamento speed
// Output: none (retargets portamento toward the fixed high frequency $17C0)
// Syntax: hb_cmd_up_ua(ua_idx, param);
static void hb_cmd_up_ua(unsigned char ua_idx, unsigned char param)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    c->portamento = param;
    c->target_freq_lo = 0xC0;
    c->target_freq_hi = 0x17;
}

// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         param — portamento speed
// Output: none (retargets portamento toward the fixed high frequency $17C0)
// Syntax: hb_cmd_up_sid(sid_idx, ch_idx, param);
static void hb_cmd_up_sid(unsigned char sid_idx, unsigned char ch_idx, unsigned char param)
{
    hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];
    c->portamento = param;
    c->target_freq_lo = 0xC0;
    c->target_freq_hi = 0x17;
}

// ---- Cmd89 Vo: volume (UA/SID; ignored on Cmd) ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — volume, low 6 bits used
// Output: none (writes hardware volume register immediately)
// Syntax: hb_cmd_vo_ua(ua_idx, param);
static void hb_cmd_vo_ua(unsigned char ua_idx, unsigned char param)
{
    volatile unsigned char *base = (volatile unsigned char *)(unsigned long)audio_ch_base[ua_idx];
    unsigned char vol = (unsigned char)(param & 0x3F);
    base[AUDIO_OFF_VOL] = vol; // immediate, active register -- matches the original's direct poke
    hb_ua[ua_idx].last_volume = vol;
}

// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         param — new sustain nibble (release untouched)
// Output: none
// Syntax: hb_cmd_vo_sid(sid_idx, ch_idx, param);
static void hb_cmd_vo_sid(unsigned char sid_idx, unsigned char ch_idx, unsigned char param)
{
    // Sets the SUSTAIN nibble of EnvSR only, keeping RELEASE untouched.
    // The 6502's 4x ASL truncates to 8 bits each step (unlike a C `int`
    // shift), so the cast below is required to match exactly.
    hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];
    unsigned char shifted = (unsigned char)(param << 4);
    c->sid_env_sr = (unsigned char)((c->sid_env_sr & 0x0F) | shifted);
}

// ---- Cmd8A Xt: nonzero param -> sync byte out; zero param -> kill/reset ----
// Input:  ua_idx — Ultimate Audio channel, 0-6; param — nonzero = sync byte,
//         0 = kill the channel
// Output: none (writes hb_ext_out, or silences the channel)
// Syntax: hb_cmd_xt_ua(ua_idx, param);
static void hb_cmd_xt_ua(unsigned char ua_idx, unsigned char param)
{
    if (param != 0) { hb_ext_out = param; return; }
    hb_ua[ua_idx].shadow_gate = 0x10;
}

// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         param — nonzero = sync byte, 0 = kill the channel
// Output: none (writes hb_ext_out, or silences the channel)
// Syntax: hb_cmd_xt_sid(sid_idx, ch_idx, param);
static void hb_cmd_xt_sid(unsigned char sid_idx, unsigned char ch_idx, unsigned char param)
{
    if (param != 0) { hb_ext_out = param; return; }
    {
        hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];
        c->sid_env_ad = 0;
        c->sid_env_sr = 0;
        c->sid_wave = 0;
        c->sid_freq_lo = 0;
        c->sid_freq_hi = 0;
        c->wave_arp_count = 0;
        c->gate_mask = 0xFE;
    }
}

// Input:  param — nonzero = sync byte, 0 = full player reset
// Output: none (writes hb_ext_out, or resets all SID/UA state)
// Syntax: hb_cmd_xt_cmd(param); // "Xt00 on Cmd channel resets everything"
static void hb_cmd_xt_cmd(unsigned char param)
{
    if (param != 0) { hb_ext_out = param; return; }
    hb_init_ua_and_sids(); // "Xt00 on Cmd channel resets everything"
}

// ---- Dispatchers -- map command index (note byte & 0x7F, valid 0-10) to
// the per-channel-type handler, matching UACommandTable/SIDCommandTable/
// CmdCommandTable's CmdNone gaps exactly. ----

// hb_ua_track_cmd — dispatches a track command byte to the right Cmd8x_*
// handler for a UA channel.
// Input:  ua_idx — Ultimate Audio channel, 0-6
//         cmd_byte — raw command byte (bit7 set, low 7 bits = command index)
// Output: none
// Syntax: hb_ua_track_cmd(ua_idx, note); // called when note&0x80 in the row buffer
static void hb_ua_track_cmd(unsigned char ua_idx, unsigned char cmd_byte)
{
    unsigned char idx = (unsigned char)(cmd_byte & 0x7F);
    unsigned char param = hb_row_buf[ua_idx * 2 + 1];

    if (idx > 0x0A)
        return;

    switch (idx)
    {
    case 0x00: hb_cmd_pa_ua(ua_idx, param); break;
    case 0x01: hb_cmd_bt(param); break;
    case 0x02: hb_cmd_dn_ua(ua_idx, param); break;
    case 0x03: hb_cmd_fi_ua(ua_idx, param); break;
    case 0x04: hb_cmd_iv_all(param); break; // Iv affects all SIDs on UA channel
    case 0x05: hb_cmd_le(param); break;
    case 0x06: break; // Co ignored on UA channel
    case 0x07: hb_cmd_po_ua(ua_idx, param); break;
    case 0x08: hb_cmd_up_ua(ua_idx, param); break;
    case 0x09: hb_cmd_vo_ua(ua_idx, param); break;
    case 0x0A: hb_cmd_xt_ua(ua_idx, param); break;
    }
}

// hb_sid_track_cmd — dispatches a track command byte to the right Cmd8x_*
// handler for a SID channel.
// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
//         cmd_byte — raw command byte (bit7 set, low 7 bits = command index)
// Output: none
// Syntax: hb_sid_track_cmd(sid_idx, ch_idx, note);
static void hb_sid_track_cmd(unsigned char sid_idx, unsigned char ch_idx, unsigned char cmd_byte)
{
    unsigned char idx = (unsigned char)(cmd_byte & 0x7F);
    unsigned char off = (unsigned char)(14 + sid_idx * 6 + ch_idx * 2);
    unsigned char param = hb_row_buf[off + 1];

    if (idx > 0x0A)
        return;

    switch (idx)
    {
    case 0x00: break; // Pa ignored on SID channel
    case 0x01: hb_cmd_bt(param); break;
    case 0x02: hb_cmd_dn_sid(sid_idx, ch_idx, param); break;
    case 0x03: hb_cmd_fi_sid(sid_idx, ch_idx, param); break;
    case 0x04: hb_cmd_iv_sid(sid_idx, param); break; // Iv affects current SID chip
    case 0x05: hb_cmd_le(param); break;
    case 0x06: hb_cmd_co(sid_idx, param); break;
    case 0x07: hb_cmd_po_sid(sid_idx, ch_idx, param); break;
    case 0x08: hb_cmd_up_sid(sid_idx, ch_idx, param); break;
    case 0x09: hb_cmd_vo_sid(sid_idx, ch_idx, param); break;
    case 0x0A: hb_cmd_xt_sid(sid_idx, ch_idx, param); break;
    }
}

// hb_cmd_channel_cmd — dispatches a track command byte on the Cmd channel
// (row byte 63), which affects tempo/volume/pattern-length/slide-all/
// sync globally rather than one specific channel.
// Input:  cmd_byte — raw command byte (bit7 set, low 7 bits = command index)
// Output: none
// Syntax: hb_cmd_channel_cmd(hb_row_buf[62]); // called when hb_row_buf[62]&0x80
static void hb_cmd_channel_cmd(unsigned char cmd_byte)
{
    unsigned char idx = (unsigned char)(cmd_byte & 0x7F);
    unsigned char param = hb_row_buf[63]; // Cmd channel's own "+1" byte

    if (idx > 0x0A)
        return;

    switch (idx)
    {
    case 0x01: hb_cmd_bt(param); break;
    case 0x02: hb_cmd_slide_all(param, 0x00, 0x01); break; // Dn on Cmd: slides ALL sounds
    case 0x04: hb_cmd_iv_all(param); break;
    case 0x05: hb_cmd_le(param); break;
    case 0x08: hb_cmd_slide_all(param, 0x00, 0x17); break; // Up on Cmd: slides ALL sounds
    case 0x0A: hb_cmd_xt_cmd(param); break;
    default: break; // Pa/Fi/Co/Po/Vo ignored on Cmd channel
    }
}

// =================================================================
// Modulations (vibrato/PWM/filter-sweep/wave-arp stepping, portamento) --
// port of Modulations/ModulateSIDandUA/ModulateChannel (player.s lines
// 1547-2013).
//
// Structural deviation from the source (verified behaviorally identical):
// the original interleaves SID-filter-modulation and UA-channel-portamento
// into a single shared loop (both are indexed with the same 32-byte stride,
// and there happen to be 7 UA channels and 8 SID chips, so one shared `x`
// loop variable drives both, with the 8th iteration skipping the UA half
// via a `cpx #$e0` check). That's a code-size trick, not a dependency --
// SID filter state and UA channel state never interact, and the actual
// hardware flush (RegisterUpdate) happens as a separate step afterwards --
// so this port uses two plain, independent loops instead for clarity.
// =================================================================

// ---------------------------------------------------------------
// hb_modulate_portamento — shared 16-bit slide-towards-target step, used
// for both the UA sample-rate frequency slide (ModulateSIDandUA's tail)
// and the SID base-frequency slide (ModulateChannel's portamento block) --
// identical arithmetic in the source for both, just different register
// names/widths.
//
// VERIFIED ASYMMETRY (traced branch-by-branch from the source, not a bug):
// sliding UP treats "new value == target" as arrived; sliding DOWN does
// NOT -- an exact landing during a down-slide is instead caught by the
// up-front equality check on the *next* tick. This costs at most one extra
// tick with the portamento-active flag still set (the register value
// itself is already correct), and is reproduced here exactly rather than
// "cleaned up".
// Input:  freq_lo/freq_hi     — pointers to the channel's current frequency
//         target_lo/target_hi — pointers to the portamento target frequency
//                                (target_hi < 0 means "not active")
//         rate                — per-tick step size
// Output: none (updates *freq_lo/*freq_hi, and disables the slide via
//         *target_hi=0xFF once arrived)
// Syntax: hb_modulate_portamento(&c->freq_lo, &c->freq_hi, &c->target_freq_lo, &c->target_freq_hi, c->portamento);
// ---------------------------------------------------------------
static void hb_modulate_portamento(unsigned char *freq_lo, unsigned char *freq_hi,
                                    unsigned char *target_lo, unsigned char *target_hi,
                                    unsigned char rate)
{
    unsigned cur, target;

    if ((signed char)*target_hi < 0)
        return; // negative target_hi = portamento not active

    cur    = ((unsigned)*freq_hi << 8)   | *freq_lo;
    target = ((unsigned)*target_hi << 8) | *target_lo;

    if (cur == target)
    {
        *target_hi = 0xFF; // already there -- disable
        return;
    }

    if (target < cur)
    {
        cur = (unsigned)(cur - rate);
        if (cur < target)          // strictly-less: source's slidedown arrival test
        {
            cur = target;
            *target_hi = 0xFF;
        }
    }
    else
    {
        cur = (unsigned)(cur + rate);
        if (cur >= target)         // >=: source's slideup arrival test (includes equality)
        {
            cur = target;
            *target_hi = 0xFF;
        }
    }

    *freq_lo = (unsigned char)cur;
    *freq_hi = (unsigned char)(cur >> 8);
}

// ---------------------------------------------------------------
// hb_cutoff_bounce_step — CutoffBounce's bit7 selects "stop at the wall"
// vs "bounce off it"; ported literally (not as a clean negate) since the
// source's own "negate rate" comment is only accurate when CutoffBounce's
// low 7 bits are 0 -- the literal `(bounce<<1) [- mod]` form is exact
// regardless of that assumption. Factored out of hb_modulate_sid_filter
// (used identically at both the top and bottom clamp) as part of working
// around an Oscar64 -O2 optimizer non-termination warning ("Optimizer
// locked in infinite loop") seen when this logic was inlined twice in a
// function with this shape -- see oscar64manual.md.
// Input:  bounce — chip's cutoff_bounce byte (bit7 = stop-vs-bounce flag)
//         mod    — current cutoff_mod rate/direction byte
// Output: the new cutoff_mod value after hitting a clamp
// Syntax: chip->cutoff_mod = hb_cutoff_bounce_step(chip->cutoff_bounce, chip->cutoff_mod);
// ---------------------------------------------------------------
static unsigned char hb_cutoff_bounce_step(unsigned char bounce, unsigned char mod)
{
    unsigned char shifted = (unsigned char)(bounce << 1);
    return (bounce & 0x80) ? shifted : (unsigned char)(shifted - mod);
}

// ---------------------------------------------------------------
// hb_modulate_sid_filter — port of ModulateSIDandUA's filter-cutoff half.
// Input:  chip — pointer to the SID chip to sweep
// Output: none (updates chip->filter_lo/filter_hi/cutoff_mod)
// Syntax: hb_modulate_sid_filter(&hb_sids[i]); // called once per chip, every tick
// ---------------------------------------------------------------
static void hb_modulate_sid_filter(hb_sid_chip_t *chip)
{
    unsigned cur, top, bottom;

    if (chip->cutoff_mod == 0)
        return;

    cur = (unsigned)((((unsigned)chip->filter_hi << 8) | chip->filter_lo)
                      + (unsigned)(signed char)chip->cutoff_mod);

    top    = ((unsigned)chip->cutoff_top_hi << 8)    | chip->cutoff_top_lo;
    bottom = ((unsigned)chip->cutoff_bottom_hi << 8) | chip->cutoff_bottom_lo;

    if (cur >= top)
    {
        chip->cutoff_mod = hb_cutoff_bounce_step(chip->cutoff_bounce, chip->cutoff_mod);
        cur = top;
        chip->filter_lo = (unsigned char)top;
        chip->filter_hi = (unsigned char)(top >> 8);

        // Source: "bne .filterdone" -- only falls through to the bottom
        // check when the top bound's hi byte is 0 (never true for real
        // filter cutoff data, which lives in the $8000-$9FFF range; kept
        // for exact fidelity anyway).
        if (chip->cutoff_top_hi != 0)
            return;
    }
    else
    {
        chip->filter_lo = (unsigned char)cur;
        chip->filter_hi = (unsigned char)(cur >> 8);
    }

    if (cur < bottom)
    {
        chip->cutoff_mod = hb_cutoff_bounce_step(chip->cutoff_bounce, chip->cutoff_mod);
        chip->filter_lo = (unsigned char)bottom;
        chip->filter_hi = (unsigned char)(bottom >> 8);
    }
}

// ---------------------------------------------------------------
// hb_modulate_ua_channel — port of ModulateSIDandUA's UA-portamento half.
// Unlike hb_play_sample_note, the rate register here is recomputed and
// rewritten to hardware every tick, unconditionally (matches the source --
// .portadone is the fallthrough for every path, not just the active-slide
// ones), reusing audio_channel_set_rate for the actual hardware write
// (same register-order convention as hb_trigger_sample()'s own UA writes).
// Input:  ua_idx — Ultimate Audio channel, 0-6
// Output: none (writes the hardware rate register every call)
// Syntax: hb_modulate_ua_channel(ua_idx); // called once per channel, every tick
// ---------------------------------------------------------------
static void hb_modulate_ua_channel(unsigned char ua_idx)
{
    hb_ua_channel_t *c = &hb_ua[ua_idx];
    unsigned withfinetune, rate;

    hb_modulate_portamento(&c->freq_lo, &c->freq_hi,
                            &c->target_freq_lo, &c->target_freq_hi,
                            c->portamento);

    withfinetune = (unsigned)((((unsigned)c->freq_hi << 8) | c->freq_lo)
                             + (((unsigned)c->finetune_hi << 8) | c->finetune_lo));

    rate = hb_get_ultimate_freq((unsigned char)(withfinetune >> 8), (unsigned char)withfinetune);
    audio_channel_set_rate(ua_idx, rate);
}

// ---------------------------------------------------------------
// hb_modulate_channel — port of ModulateChannel: wave/arp table stepping,
// PWM sweep, SID base-frequency portamento, vibrato, and the final
// arpeggio+vibrato+finetune -> SID frequency register calculation. Runs
// unconditionally every tick for every SID channel (matches the source --
// there's no "is this channel playing" gate here; silent channels just
// modulate silently until their next note-trigger overwrites the state).
// Input:  sid_idx, ch_idx — SID chip (0-7) and channel within it (0-2)
// Output: none (updates hb_sids[sid_idx].ch[ch_idx]'s active register image)
// Syntax: hb_modulate_channel(sid_idx, ch_idx); // called once per channel, every tick
// ---------------------------------------------------------------
static void hb_modulate_channel(unsigned char sid_idx, unsigned char ch_idx)
{
    hb_sid_channel_t *c = &hb_sids[sid_idx].ch[ch_idx];

    // ---- wave/arp table stepping ----
    if (c->wave_arp_count != 0 && c->instr_hi != 0)
    {
        if (--c->wave_arp_count == 0)
        {
            hb_inst_params_t *inst = &hb_songdata.inst_params[c->instr_lo];
            unsigned char idx, w;

            for (;;)
            {
                c->wave_arp_step++;                     // free-running counter (inc WaveArpStep,x)
                c->wave_arp_count = c->wave_arp_speed;   // reset step timer

                idx = (unsigned char)(c->wave_arp_step & 0x0F);
                w = inst->wave_table[idx];

                if (w == 0) { c->wave_arp_count = 0; break; } // end of table

                if (w == 0xFF) // loop command -> jump to the step named in the arp table
                {
                    idx = (unsigned char)(inst->arp_table[idx] & 0x0F);
                    c->wave_arp_step = idx; // loop target overwrites the free-running counter
                    w = inst->wave_table[idx];
                    if (w == 0) { c->wave_arp_count = 0; break; }
                    // The resolved value is NOT re-checked for a second $FF
                    // here -- matches the source exactly; a loop-to-loop is
                    // instead caught by the w==0xFF check just below.
                }

                if (w == 0xFC) { c->sid_env_ad     = inst->arp_table[idx]; continue; }
                if (w == 0xFD) { c->sid_env_sr     = inst->arp_table[idx]; continue; }
                if (w == 0xFE) { c->wave_arp_speed = inst->arp_table[idx]; continue; }
                if (w == 0xFF) break; // loop-to-loop guard: treat as end-of-table

                c->sid_wave = w;
                c->current_arp = inst->arp_table[idx];
                break;
            }
        }
    }

    // ---- pulse width modulation ----
    {
        unsigned rate = ((unsigned)c->pwm_rate_hi << 8) | c->pwm_rate;
        if (rate != 0)
        {
            unsigned pw     = ((unsigned)c->sid_pw_hi << 8) | c->sid_pw_lo;
            unsigned top    = ((unsigned)c->pwm_top_hi << 8) | 0xFF; // low bound byte is always $FF (source: "cmp #$ff")
            unsigned bottom = ((unsigned)c->pwm_bottom_hi << 8);     // low bound byte is always $00

            pw = (unsigned)(pw + rate);
            c->sid_pw_lo = (unsigned char)pw;
            c->sid_pw_hi = (unsigned char)(pw >> 8);

            if (pw >= top)
            {
                unsigned neg = (unsigned)(0 - rate);
                c->pwm_rate    = (unsigned char)neg;
                c->pwm_rate_hi = (unsigned char)(neg >> 8);
                c->sid_pw_hi = (unsigned char)(top >> 8);
                c->sid_pw_lo = 0xFF;
            }
            else if (pw < bottom)
            {
                unsigned neg = (unsigned)(0 - rate);
                c->pwm_rate    = (unsigned char)neg;
                c->pwm_rate_hi = (unsigned char)(neg >> 8);
                c->sid_pw_hi = (unsigned char)(bottom >> 8);
                c->sid_pw_lo = 0x00;
            }
        }
    }

    // ---- SID base-frequency portamento ----
    hb_modulate_portamento(&c->base_freq_lo, &c->base_freq_hi,
                            &c->target_freq_lo, &c->target_freq_hi,
                            c->portamento);

    // ---- vibrato ----
    {
        unsigned char run_this_tick;

        if (c->vib_delay == 0)
            run_this_tick = 1;
        else
        {
            c->vib_delay--;
            run_this_tick = (c->vib_delay == 0);
        }

        if (run_this_tick)
        {
            unsigned sum;
            unsigned char abs_phase;

            sum = (unsigned)c->vib_frac + c->vib_rate;
            c->vib_frac = (unsigned char)sum;
            c->vib_phase = (unsigned char)(c->vib_phase + c->vib_rate_hi + (unsigned char)(sum >> 8));

            abs_phase = (c->vib_phase & 0x80) ? (unsigned char)(~c->vib_phase + 1) : c->vib_phase;

            if (abs_phase >= c->vib_width)
            {
                unsigned neg = (unsigned)(0 - (((unsigned)c->vib_rate_hi << 8) | c->vib_rate));
                c->vib_rate    = (unsigned char)neg;
                c->vib_rate_hi = (unsigned char)(neg >> 8);

                // bounce back: apply the now-reversed rate once more, same tick
                sum = (unsigned)c->vib_frac + c->vib_rate;
                c->vib_frac = (unsigned char)sum;
                c->vib_phase = (unsigned char)(c->vib_phase + c->vib_rate_hi + (unsigned char)(sum >> 8));
            }
        }
    }

    // ---- arpeggio + vibrato + finetune -> final SID frequency register ----
    {
        unsigned char arp = c->current_arp;
        unsigned char absolute = (arp & 0x80) != 0;
        unsigned temp = ((unsigned)(arp & 0x7F)) << 6; // arp note * 64, same fixed-point unit as base_freq

        if (!absolute)
        {
            unsigned base = ((unsigned)c->base_freq_hi << 8) | c->base_freq_lo;
            unsigned vib  = (unsigned)(signed char)c->vib_phase; // sign-extended 16-bit add
            unsigned ft   = ((unsigned)c->finetune_hi << 8) | c->finetune;

            temp = (unsigned)(temp + base + vib + ft);
        }

        {
            unsigned freq = hb_get_sid_freq((unsigned char)(temp >> 8), (unsigned char)temp);
            c->sid_freq_lo = (unsigned char)freq;
            c->sid_freq_hi = (unsigned char)(freq >> 8);
        }

        c->sid_wave = (unsigned char)(c->sid_wave & c->gate_mask); // key-up gate mask, re-applied every tick
    }
}

// ---------------------------------------------------------------
// hb_modulations — port of Modulations's top-level dispatch loop. See the
// "Modulations" section comment above for why this is two plain loops
// instead of the source's one shared-index loop.
// Input:  none
// Output: none (updates every SID/UA channel's modulation state)
// Syntax: hb_modulations(); // called by hb_tick() every tick, unconditionally
// ---------------------------------------------------------------
static void hb_modulations(void)
{
    unsigned char sid_idx, ua_idx, ch_idx;

    for (sid_idx = 0; sid_idx < HB_MAX_SIDS; sid_idx++)
    {
        hb_modulate_sid_filter(&hb_sids[sid_idx]);
        for (ch_idx = 0; ch_idx < 3; ch_idx++)
            hb_modulate_channel(sid_idx, ch_idx);
    }

    for (ua_idx = 0; ua_idx < HB_UA_CHANNELS; ua_idx++)
        hb_modulate_ua_channel(ua_idx);
}

// =================================================================
// PlayFX / StopFX (manual sample trigger, outside song playback)
// =================================================================

// ---------------------------------------------------------------
// hb_play_fx — port of PlayFX. Validates the channel, then reproduces
// PlayFX's ".play" (note+9, sample-1, channel lookup) before jumping into
// the SAME hb_trigger_sample() continuation a row-triggered UA note uses
// (matches the original's `jmp PlaySampleNote.editorentry` -- PlayFX has
// no row buffer or tied-note concept, so it skips straight to the shared
// "compute sample pointer onward" logic, including the sample's own
// note_pitch/transpose application).
//
// sei/cli (not php/plp): matches the original exactly -- PlayFX is a
// public API called from outside the tick IRQ (unlike e.g. hb_set_tempo,
// which can be called from within it via a Bt command and must preserve
// the caller's interrupt state), so unconditionally enabling interrupts
// on exit is correct here.
// See hbplayer.h for the full Input/Output/Syntax.
// ---------------------------------------------------------------
void hb_play_fx(unsigned char ch, unsigned char sample, unsigned char note)
{
    if (ch >= 7)
        return;

    __asm { sei }
    hb_trigger_sample(ch, (unsigned char)(note + 9), (unsigned char)(sample - 1));
    __asm { cli }
}

// ---------------------------------------------------------------
// hb_stop_fx — port of StopFX. See hbplayer.h for the full Input/Output/Syntax.
// ---------------------------------------------------------------
void hb_stop_fx(unsigned char ch)
{
    if (ch >= 7)
        return;

    __asm { sei }
    hb_stop_sample_note(ch);
    __asm { cli }
}
