// heartbeat-demo — screen output helpers
//
// petscii.h MUST be included: cwin writes bytes directly to screen
// RAM (no p2s conversion via #pragma native).  With petscii.h the
// charmap remaps source bytes so that source uppercase → uppercase
// display and source lowercase → lowercase display in the
// lowercase+uppercase charset ($1800 char ROM).

#include <c64/charwin.h>
#include <c64/vic.h>
#include <petscii.h>
#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "screen.h"

#pragma code(code)
#pragma data(data)

// Global CharWin
CharWin cw;

static char p2smap[] = {0x40, 0x00, 0x40, 0x20, 0x40, 0xc0, 0x80, 0x80};

// p2s — converts one PETSCII source byte to its screen-code equivalent
// (the same transform petscii.h's charmap applies to string literals),
// needed here because screen_header_line() builds a raw screen-code
// buffer by hand instead of going through the normal charmap path.
// Input:  ch — one PETSCII source byte
// Output: the equivalent raw screen code
// Syntax: unsigned char sc = p2s('A');
static inline char p2s(char ch)
{
  return ch ^ p2smap[ch >> 5];
}

// ---------------------------------------------------------------
// screen_header_line — draw one reversed-video, centered header line.
// Exported (not just screen_init()'s internal helper) so other screens
// (e.g. src/visualizer.c) can draw a matching header without duplicating
// the p2s/reverse-video-bit logic.
// Input:  row   — screen row 0-24 to draw on
//         text  — text to center (source case; converted via p2s())
//         color — text color (a COL_*/VCOL_* constant)
// Output: none
// Syntax: screen_header_line(0, "Heartbeat Tracker Player Demo", COL_HEADER1);
// ---------------------------------------------------------------
void screen_header_line(char row, const char *text, char color) {
    char buf[41];
    char len = (char)strlen(text);
    char start = (char)((40 - len) / 2);
    char i;

    cwin_fill_rect_raw(&cw, 0, row, 40, 1, SC_REVSPACE, color);

    for (i = 0; i < len && i < 40; i++) {
        unsigned char c = p2s((unsigned char)text[i]);
        buf[i] = (char)(c | 0x80);
    }
    buf[i] = 0;
    cwin_putat_string_raw(&cw, start, row, buf, color);
}

// ---------------------------------------------------------------
// screen_init — initialise VIC text mode (bank 0), clear the screen, draw
// the two-line header.
// Input:  subtitle — text for header line 1
// Output: none
// Syntax: screen_init("Hardware Detection  v1.0.0");
// ---------------------------------------------------------------
void screen_init(const char *subtitle) {
    // Lowercase+uppercase charset at VIC-II bank-0 address $1800
    vic_setmode(VICM_TEXT, (char *)0x0400, (char *)0x1800);
    vic.color_border = COL_BORDER;
    vic.color_back   = COL_BACKGROUND;

    cwin_init(&cw, (char *)0x0400, 0, 0, 40, 25);
    cwin_clear(&cw);

    // With petscii.h: source mixed-case → correct mixed-case display
    screen_header_line(0, "Heartbeat Tracker Player Demo", COL_HEADER1);
    screen_header_line(1, subtitle,         COL_HEADER2);

    cwin_cursor_move(&cw, 0, 3);
}

// ---------------------------------------------------------------
// screen_result — print one detection result line ("  LABEL : [ OK ] detail"
// or "  LABEL : [FAIL] detail"), then advance to the next line.
// Input:  label  — short label, e.g. "REU  "
//         ok     — nonzero for "[ OK ]", 0 for "[FAIL]"
//         detail — extra text shown after the badge
// Output: none
// Syntax: screen_result("REU  ", 1, "16 MB");
// ---------------------------------------------------------------
void screen_result(const char *label, char ok, const char *detail) {
    char badge_col  = ok ? COL_OK   : COL_FAIL;
    char detail_col = ok ? COL_DETAIL_OK : COL_DETAIL_FAIL;
    const char *badge = ok ? "[ OK ]" : "[Fail]";

    cwin_put_string(&cw, "  ",   COL_LABEL);
    cwin_put_string(&cw, label,  COL_LABEL);
    cwin_put_string(&cw, " : ",  COL_LABEL);
    cwin_put_string(&cw, badge,  badge_col);
    cwin_put_string(&cw, "  ",   detail_col);
    cwin_put_string(&cw, detail, detail_col);
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_info — print a plain information line in COL_INFO, then newline.
// Input:  msg — text to print
// Output: none
// Syntax: screen_info("Checking turbo mode...");
// ---------------------------------------------------------------
void screen_info(const char *msg) {
    cwin_put_string(&cw, msg, COL_INFO);
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_hint — print a hint/action line ("  -> msg") in COL_HINT, then
// newline. Keep msg <= 35 chars to fit in the 40-col screen.
// Input:  msg — hint text
// Output: none
// Syntax: screen_hint("Enable turbo in Ultimate firmware");
// ---------------------------------------------------------------
void screen_hint(const char *msg) {
    cwin_put_string(&cw, "  -> ", COL_HINT);
    cwin_put_string(&cw, msg,     COL_HINT);
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_blank_line — output one blank line.
// Input:  none
// Output: none
// Syntax: screen_blank_line();
// ---------------------------------------------------------------
void screen_blank_line(void) {
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_error_exit — shows error, waits for key, RETURNS. Caller must
// immediately do `return 1`. Note: the exit string is intentionally
// distinct from screen_wait_key's to prevent Oscar64 suffix-merging
// adjacent string literals.
// Input:  msg  — error message
//         hint — optional follow-up hint, or NULL/empty for none
// Output: none (see caller-contract note above)
// Syntax: screen_error_exit("No UCI found.", "F2 > UCI Settings > Enable"); return 1;
// ---------------------------------------------------------------
void screen_error_exit(const char *msg, const char *hint) {
    screen_blank_line();
    cwin_put_string(&cw, msg, COL_FAIL);
    cwin_cursor_newline(&cw);
    if (hint && hint[0]) {
        screen_hint(hint);
    }
    screen_blank_line();
    cwin_put_string(&cw, "Press a key - exit to BASIC.", COL_KEY);
    cwin_cursor_newline(&cw);
    cwin_getch();
}

// ---------------------------------------------------------------
// screen_wait_key — print msg (or a default prompt) and wait for a
// keypress, debounced against a key already held from a prior screen.
// Input:  msg — prompt text, or NULL/empty for "Press any key to continue."
// Output: none
// Syntax: screen_wait_key("Press any key to start playback.");
// ---------------------------------------------------------------
void screen_wait_key(const char *msg) {
    char stable;
    const char *text = (msg && msg[0]) ? msg : "Press any key to continue.";
    cwin_put_string(&cw, text, COL_KEY);
    cwin_cursor_newline(&cw);
    // Debounce via direct CIA1 matrix read: require 4 consecutive VSync frames
    // with no key held before polling for a new press.  A single spin-until-FF
    // loop is not enough — mechanical bounce or a key held from a prior scene
    // produces a brief FF glitch that exits the old loop, then the press-detect
    // loop immediately fires on the still-held key.
    stable = 0;
    while (stable < 4) {
        vic_waitFrame();
        *((volatile unsigned char *)0xDC00) = 0;
        if (*((volatile unsigned char *)0xDC01) == (unsigned char)0xFF)
            stable++;
        else
            stable = 0;
        *((volatile unsigned char *)0xDC00) = (unsigned char)0xFF;
    }
    while (1) {
        *((volatile unsigned char *)0xDC00) = 0;
        if (*((volatile unsigned char *)0xDC01) != (unsigned char)0xFF) break;
        *((volatile unsigned char *)0xDC00) = (unsigned char)0xFF;
    }
    *((volatile unsigned char *)0xDC00) = (unsigned char)0xFF;
}
