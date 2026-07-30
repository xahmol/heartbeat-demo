// heartbeat-demo
// Hardware detection and startup for the Ultimate 64
// Use MMAP_NO_BASIC ($36) throughout: KERNAL+I/O visible, $A000-$BFFF always RAM.
//
// Four separate regions instead of one $0A00-$C000 blob, so the visualiser's
// manually-placed VIC bank 2 screen/charset/sprite data (src/visualizer.c,
// $A000-$BFFF) can never collide with ordinary code/data/bss/heap/stack:
//
//   - $0A00-$9980: normal code+data+bss (measured usage ~34.5KB and growing
//     as the visualiser gains features -- verify against build/*.map's
//     BSSEnd after any change that adds meaningfully-sized new statics; the
//     margin has been tight before, see git history).
//   - $9980-$9A80: heap (heapsize(256) below).
//   - $9A80-$9E80: stack (stacksize(0x400) below).
//   - $9E80-$A000: small unused gap (384 bytes, rounding leftover).
//   - $A000-$BFFF: reserved here, used directly by visualizer.c.
//
// $9000-$9FFF (part of `main` and the heap/stack regions above) is the VIC
// character-ROM-shadow range, but that only affects the VIC chip's OWN
// char/bitmap fetches when bank 0 or 2 is selected -- ordinary CPU code/
// data placement there is unaffected and safe, which is why `main` is
// allowed to extend through it instead of stopping at $8FFF like an
// earlier, overly-conservative version of this layout did.
//
// This fixes a real, confirmed bug: with a single undivided region, Oscar64
// happily placed ordinary BSS globals (hb_sids -- live SID channel state,
// written every tick -- landed at $8A6A) inside what visualizer.c assumed
// was "free" VIC bank 2 scratch space, based on nothing but a BSSEnd
// snapshot. The visualiser's charset copy then clobbered hb_sids the
// instant it ran, killing SID playback outright. It went unnoticed through
// several milestones only because until then something inert happened to
// land there instead -- sheer luck, not a property of the design. Region
// pragmas are the only way to make the linker itself enforce the split;
// never trust an address range is free just because nothing currently
// visible in the map claims it.
//
// Note this also debunks the ORIGINAL assumption behind $8000-$8FFF/
// $A000-$AFFF specifically: heap/stack apparently claim the entire
// remaining region tail regardless of heapsize()/stacksize() values,
// exactly like stack alone used to before -- so with a single region there
// was in fact no genuinely free window anywhere in $8000-$BFFF at all, only
// the small explicit heap/stack regions below plus $A000-$BFFF freed up by
// excluding it from `main` altogether.
#pragma region(main,  0x0A00, 0x9980, , , {code, data, bss})
#pragma region(rheap, 0x9980, 0x9A80, , , {heap})
#pragma region(rstack, 0x9A80, 0x9E80, , , {stack})
#pragma heapsize(256)
// Explicit stack size: without this, Oscar64 defaults to filling ALL
// remaining region space with the stack (observed: stack reserved from
// ~$7CD0 to ~$BF26, ~17KB), leaving no free space in VIC banks 1/2 for the
// visualiser's custom charset/sprite assets (see ARCHITECTURE.md's memory
// layout notes). This program has no deep/recursive call chains -- 1KB is
// a generous margin over anything actually observed, freeing the rest for
// explicit reuse. Verify against build/*.map's StackEnd/HeapEnd after any
// change here, and re-run the zero-page + stack-safety check described in
// ARCHITECTURE.md before trusting a build that changes this.
#pragma stacksize(0x400)
// Written in 2026 by Xander Mol
//
// petscii.h is required: with the lowercase+uppercase charset and
// petscii.h's charmap, source uppercase → uppercase display and
// source lowercase → lowercase display.  Write strings in normal
// English case; the compiler remaps them to the correct PETSCII
// bytes for on-screen display.

#include <c64/charwin.h>
#include <c64/vic.h>
#include <c64/cia.h>
#include <petscii.h>
#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "ultimate_common_lib.h"
#include "ultimate_dos_lib.h"
#include "screen.h"
#include "detect.h"
#include "turbo.h"
#include "hbplayer.h"
#include "visualizer.h"

#ifndef VERSION
#define VERSION "v0.1.0-dev"
#endif

// Heartbeat test song filenames, on any SD/USB drive's idi8b/heartbeat-demo/
// (see Makefile's INSTALL_PATH — must match hb_load()'s internal search path).
// vis_song_files[]/vis_song_index (visualizer.h) are the shared song table;
// this loads index 0 at startup. HB_SONG_REU_BASE is in hbplayer.h (shared
// with visualizer.c's runtime song-switch reload).

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

static void fmt_dec(char *buf, unsigned char val)
{
    char i = 0;
    if (val >= 100)
    {
        buf[i++] = (char)('0' + val / 100);
        val %= 100;
    }
    buf[i++] = (char)('0' + val / 10);
    buf[i++] = (char)('0' + val % 10);
    buf[i] = 0;
}

// Copy UCI response string, uppercasing and filtering to printable ASCII.
// petscii.h remaps source letters, so UCI data (raw ASCII) must be
// converted explicitly to PETSCII uppercase for correct display.
static char uci_to_upper(char *dst, char maxlen)
{
    char i, j = 0;
    for (i = 0; i < 127 && j < maxlen; i++)
    {
        unsigned char c = (unsigned char)uii_data[(unsigned char)i];
        if (c == 0)
            break;
        if (c >= 32 && c <= 126)
        { // printable ASCII
            // Force uppercase: in raw PETSCII, 0x61-0x7A are uppercase A-Z
            // (without petscii.h remapping), so map ASCII a-z to that range
            if (c >= 0x61 && c <= 0x7A)   // raw hex: immune to petscii.h charmap
                c = (unsigned char)(c - 0x20);
            dst[(unsigned char)j++] = (char)c;
        }
    }
    dst[(unsigned char)j] = 0;
    return j;
}

// ---------------------------------------------------------------
// NMI handler — prevents RESTORE key from resetting the demo.
// ---------------------------------------------------------------
__hwinterrupt void nmi_handler(void) {}

// ---------------------------------------------------------------
// int main
// ---------------------------------------------------------------
int main(void)
{
    // MMAP_NO_BASIC ($36): KERNAL + I/O visible, BASIC ROM removed.
    // $A000-$BFFF is always CPU-accessible RAM (no BASIC ROM shadow).
    // A previous crash may have left $01=$34 (MMAP_RAM), breaking I/O.
    *((volatile unsigned char *)0x01) = 0x36;

    // Install NMI handler so RESTORE key is ignored during demo
    *((void **)0x0318) = nmi_handler;

    // Patch UDTIM vector ($0310) to RTS so the KERNAL IRQ chain's JSR $0310
    // returns safely. Without BASIC ROM, $0310 holds JMP $B248 where $B248
    // is Oscar64 RAM whose contents change with every build; the KERNAL
    // keyboard-scan path ($EA31 → $FFEA → JSR $0310 → JMP $B248) hits
    // whatever bytes happen to be there — often an illegal opcode → crash.
    *((unsigned char *)0x0310) = 0x60;  // RTS

    // Patch CBINV vector ($A002-$A003) to point to our RTS stub at $0310.
    // KERNAL UDTIM unconditionally calls JMP ($A002) on every IRQ tick.
    // With MMAP_NO_BASIC ($01=$36), $A002 is DRAM; the U64 pre-initialises
    // DRAM at $A000-$BFFF with BASIC ROM content, so $A002/$A003 = $E37B
    // (KERNAL CBINV handler).  That handler calls JSR $A67A (BASIC ROM in
    // DRAM), which hard-resets SP to $FA and executes CLI — corrupting the
    // 6502 hardware stack and re-enabling IRQs mid-IRQ.  Redirect to $0310
    // (already RTS) so JMP ($A002) returns harmlessly via the RTS stub.
    *((unsigned char *)0xA002) = 0x10;  // lo byte of $0310
    *((unsigned char *)0xA003) = 0x03;  // hi byte of $0310

    // NOTE: visualizer.c's screen memory sits at $A000-$A3E7, overlapping
    // this exact $A002/$A003 patch -- safe ONLY because hb_init() (called
    // once, before the visualiser screen ever initializes) permanently
    // replaces the KERNAL IRQ vector at $0314/$0315 with hb_irq and nothing
    // ever restores it, so the KERNAL's own UDTIM/JMP($A002) path never
    // executes again after that point. If any future change reinstalls the
    // default KERNAL IRQ vector after hb_init() (e.g. for a return-to-BASIC
    // path that doesn't just reset the machine), this stops being true and
    // $A002/$A003 must be re-verified before touching this screen address.

    // Reset CIA1 Timer A to 50 Hz PAL keyboard scan rate.
    cia1.icr = 0x7F;       // mask all CIA1 interrupts
    cia1.ta  = 0x4D25;     // 19749 counts ≈ 985248 Hz / 50 Hz (PAL)
    cia1.icr = 0x81;       // re-enable Timer A interrupt
    cia1.cra = 0x01;       // start Timer A, continuous

    char detail[26];
    char playback_started = 0; // set once visualizer_run() has taken over the screen

    // Subtitle: uppercase abbreviation + version via string concat.
    screen_init("Hardware Detection  " VERSION);

    // ---- UCI ---------------------------------------------------
    screen_info("Waiting for Ultimate firmware...");

    if (!detect_uci())
    {
        screen_result("UCI  ", 0, "Not detected");
        screen_error_exit(
            "No Ultimate Command Interface found.",
            "F2 > UCI Settings > Enable");
        return 1;
    }

    // DOS version string via uii_identify()
    uii_identify();
    if (UII_SUCCESS && uii_data[0])
        uci_to_upper(detail, 24);
    else
        strcpy(detail, "UCI Ok");
    screen_result("UCI  ", 1, detail);

    // Hardware device name via uii_get_hwinfo(0).
    uii_get_hwinfo(0);
    if (UII_SUCCESS && uci_to_upper(detail, 24) > 0)
    {
        cwin_put_string(&cw, "  Type  : ", COL_LABEL);
        cwin_put_string(&cw, detail, COL_DETAIL_OK);
        cwin_cursor_newline(&cw);
    }

    // ---- REU ---------------------------------------------------
    // Heartbeat song data streams from REU (patterns, samples), so
    // the full 16 MB is required, same as the Heartbeat standalone player.
    screen_info("Checking REU...");

    {
        unsigned char reu_mb = detect_reu();
        if (reu_mb < 16)
        {
            screen_result("REU  ", 0,
                          reu_mb == 0 ? "Not detected" : "Too small (need 16 MB)");
            screen_error_exit(
                "16 MB REU is required.",
                "F2 > C64 settings > REU > 16 MB");
            return 1;
        }
    }
    screen_result("REU  ", 1, "16 MB");

    // ---- Turbo -------------------------------------------------
    screen_info("Checking turbo mode...");

    if (!detect_turbo())
    {
        screen_result("Turbo", 0, "Not detected (1 MHz)");
        screen_hint("Enable turbo in Ultimate firmware");
    }
    else
    {
        if (detected_turbo_class == TURBO_64MHZ)
            strcpy(detail, "64 MHz");
        else if (detected_turbo_class == TURBO_48MHZ)
            strcpy(detail, "Turbo (48 MHz class)");
        else
            strcpy(detail, "Turbo");
        screen_result("Turbo", 1, detail);
    }

    // ---- Audio -------------------------------------------------
    screen_info("Checking Ultimate Audio...");

    if (!detect_audio())
    {
        screen_result("Audio", 0, "Module not found");
        screen_hint("F2 > C64/Cart settings > Audio");
    }
    else
    {
        char vbuf[4];
        fmt_dec(vbuf, detected_audio_version);
        strcpy(detail, "v");
        strcat(detail, vbuf);
        screen_result("Audio", 1, detail);
    }

    // ---- Heartbeat player: load test song, then play it ----------
    if (detected_audio_version > 0)
    {
        char buf[40];
        char song_loaded;
        screen_info("Loading Heartbeat song...");

        song_loaded = hb_load(vis_song_files[vis_song_index], HB_SONG_REU_BASE);
        if (song_loaded)
        {
            sprintf(buf, "tempo %u, %u SIDs",
                    hb_songdata.starting_tempo, HB_MAX_SIDS);
            screen_result("Song ", 1, buf);
        }
        else
        {
            screen_result("Song ", 0, "Not found");
            screen_hint("Place .reu in idi8b/heartbeat-demo/");
        }

        // ---- Start playback (Phases 3-9, all hardware-verified) --------
        if (song_loaded)
        {
            // Engage turbo before playback starts. detect_turbo() (above)
            // calls turbo_detect(), which explicitly RESTORES $D031 to
            // 1 MHz after measuring (see turbo.h) -- so without this call,
            // hb_tick would run at 1 MHz regardless of firmware settings.
            // hb_tick's per-tick budget is fixed in REAL time by the CIA1
            // Timer A period (~5.08 ms @ PAL, independent of CPU speed);
            // Phase 9's full Modulations pass (24 SID channels + 7 UA
            // channels, every tick) does not reliably fit in the ~5000 CPU
            // cycles that period gives at 1 MHz, causing dropped/coalesced
            // ticks -- audible as playback running far slower than the
            // song's real tempo. Matches the reference player's own
            // main.s, which boots directly at 16 MHz turbo for the same
            // reason (real-time REU streaming + modulation headroom).
            if (detected_turbo_class != TURBO_NOT_PRESENT)
                turbo_fast();

            hb_detect_ntsc(); // must run before hb_init(), but doesn't itself start
                              // playback -- fine to do while still on the detection screen

            // ---- Detection complete, hand off to the visualiser --------
            screen_blank_line();
            screen_info("Detection complete.");
            screen_blank_line();
            screen_wait_key("Press any key to start playback.");

            // hb_init(0,1) is what actually starts playback (installs the
            // tick IRQ with play_mode=1) -- deliberately deferred until
            // AFTER the keypress above, so music doesn't start while the
            // user is still reading the detection screen.
            hb_init(0, 1);

            // visualizer_run() owns the screen from here: note visualiser
            // + the buttons.s-equivalent test harness (Space=restart,
            // Stop=silence, A-O=PlayFX 1-15 on UA channel 6, X=StopFX
            // channel 6), until RETURN is pressed.
            visualizer_run();

            hb_stop_all();
            playback_started = 1;
        }
    }

    // ---- Detection complete (only shown if playback never started --
    // visualizer_run() already had its own exit prompt/keypress) --------
    if (!playback_started)
    {
        screen_blank_line();
        screen_info("Detection complete.");
        screen_blank_line();
        screen_wait_key(NULL);
    }

    // Wait for exit key to be fully released before returning to BASIC.
    do {
        *((volatile unsigned char *)0xDC00) = 0;
    } while (*((volatile unsigned char *)0xDC01) != (unsigned char)0xFF);
    *((volatile unsigned char *)0xDC00) = (unsigned char)0xFF;

    // Wipe keyboard buffer so any residual KERNAL scans don't inject chars.
    *((volatile unsigned char *)0xC6) = 0;

    // Restore standard C64 colors before returning to BASIC
    vic.color_border = VCOL_LT_BLUE;
    vic.color_back   = VCOL_BLUE;

    return 0;
}
