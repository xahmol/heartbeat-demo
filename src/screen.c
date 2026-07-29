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

static inline char p2s(char ch)
{
  return ch ^ p2smap[ch >> 5];
}

// ---------------------------------------------------------------
// Internal: draw one reversed-video header line.
// ---------------------------------------------------------------
static void header_line(char row, const char *text, char color) {
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
// screen_init
// ---------------------------------------------------------------
void screen_init(const char *subtitle) {
    // Lowercase+uppercase charset at VIC-II bank-0 address $1800
    vic_setmode(VICM_TEXT, (char *)0x0400, (char *)0x1800);
    vic.color_border = COL_BORDER;
    vic.color_back   = COL_BACKGROUND;

    cwin_init(&cw, (char *)0x0400, 0, 0, 40, 25);
    cwin_clear(&cw);

    // With petscii.h: source mixed-case → correct mixed-case display
    header_line(0, "heartbeat-demo", COL_HEADER1);
    header_line(1, subtitle,         COL_HEADER2);

    cwin_cursor_move(&cw, 0, 3);
}

// ---------------------------------------------------------------
// screen_result
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
// screen_info
// ---------------------------------------------------------------
void screen_info(const char *msg) {
    cwin_put_string(&cw, msg, COL_INFO);
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_hint  — keep msg <= 35 chars to fit in 40-col screen
// ---------------------------------------------------------------
void screen_hint(const char *msg) {
    cwin_put_string(&cw, "  -> ", COL_HINT);
    cwin_put_string(&cw, msg,     COL_HINT);
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_blank_line
// ---------------------------------------------------------------
void screen_blank_line(void) {
    cwin_cursor_newline(&cw);
}

// ---------------------------------------------------------------
// screen_error_exit — shows error, waits for key, RETURNS.
// Caller must immediately do `return 1`.
// Note: the exit string is intentionally distinct from screen_wait_key
// to prevent Oscar64 suffix-merging adjacent string literals.
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
// screen_wait_key
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
