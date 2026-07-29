// heartbeat-demo
// Hardware detection and startup for the Ultimate 64
// Use MMAP_NO_BASIC ($36) throughout: KERNAL+I/O visible, $A000-$BFFF always RAM.
// Region extends to $C000 so code+data+bss+stack fit safely below the MC screen at $C000.
#pragma region(main, 0x0A00, 0xC000, , , {code, data, bss, heap, stack})
#pragma heapsize(256)
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

#ifndef VERSION
#define VERSION "v0.1.0-dev"
#endif

// Heartbeat test song filename, on any SD/USB drive's idi8b/heartbeat-demo/
// (see Makefile's INSTALL_PATH — must match hb_load()'s internal search path).
// Identity charmap override: petscii.h's global charmap would otherwise remap
// these path bytes to the wrong PETSCII case for UCI's raw-ASCII filesystem protocol.
#pragma charmap(97, 97, 26)   // a-z -> a-z (identity, overrides petscii.h)
#pragma charmap(65, 65, 26)   // A-Z -> A-Z (identity)
static char hb_song_file[] = "Knight Rider Theme.reu";
#pragma charmap(97, 65, 26)   // restore petscii.h: a-z -> A-Z
#pragma charmap(65, 97, 26)   // restore petscii.h: A-Z -> a-z
#define HB_SONG_REU_BASE  0x000000UL

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

    // Reset CIA1 Timer A to 50 Hz PAL keyboard scan rate.
    cia1.icr = 0x7F;       // mask all CIA1 interrupts
    cia1.ta  = 0x4D25;     // 19749 counts ≈ 985248 Hz / 50 Hz (PAL)
    cia1.icr = 0x81;       // re-enable Timer A interrupt
    cia1.cra = 0x01;       // start Timer A, continuous

    char detail[26];

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

    // ---- Heartbeat player: load test song -----------------------
    // Struct-layout and #embed-table correctness (Phase 1) and the song
    // loader (Phase 2) have both been hardware-verified already — see
    // port plan / oscar64manual.md. Keep just a one-line confirmation here
    // so the 40x25 screen doesn't overflow; playback itself lands in later
    // phases.
    if (detected_audio_version > 0)
    {
        char buf[40];
        screen_info("Loading Heartbeat song...");

        if (hb_load(hb_song_file, HB_SONG_REU_BASE))
        {
            sprintf(buf, "Loaded, tempo %u, %u SIDs",
                    hb_songdata.starting_tempo, HB_MAX_SIDS);
            screen_result("Song ", 1, buf);
        }
        else
        {
            screen_result("Song ", 0, "Not found");
            screen_hint("Place .reu in idi8b/heartbeat-demo/");
        }
    }

    // ---- Detection complete ------------------------------------
    screen_blank_line();
    screen_info("Detection complete.");
    screen_blank_line();
    screen_wait_key(NULL);

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
