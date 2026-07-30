// heartbeat-demo — note visualiser + buttons.s-equivalent test harness
// See visualizer.h for the overview.

#include <c64/charwin.h>
#include <c64/vic.h>
#include <petscii.h>
#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "screen.h"
#include "hbplayer.h"
#include "visualizer.h"

// ---------------------------------------------------------------
// Layout
// ---------------------------------------------------------------
#define VIS_ROWS_START   3    // first channel-bar row (after the 2-line header + 1 blank)
#define VIS_MAX_ROWS     18   // rows VIS_ROWS_START .. VIS_ROWS_START+VIS_MAX_ROWS-1 (3..20)
#define VIS_LABEL_WIDTH  4    // "UA-6" / "S8-3" -- always exactly 4 chars
#define VIS_BAR_X        (VIS_LABEL_WIDTH + 1)
#define VIS_BAR_WIDTH     (40 - VIS_BAR_X)

// vis_channel_row[hb_vis channel number 0-30] -> row index 0..VIS_MAX_ROWS-1,
// or 0xFF if that channel isn't displayed (not populated, or ran out of rows).
static unsigned char vis_channel_row[31];
static unsigned char vis_row_level[VIS_MAX_ROWS];
static unsigned char vis_row_count;

// ---------------------------------------------------------------
// vis_getin — raw KERNAL GETIN, same rationale as the harness's previous
// home in main.c: bypasses conio.h's getchx()/convch() to avoid fighting
// this file's petscii.h charmap when comparing against raw key codes.
// ---------------------------------------------------------------
static unsigned char vis_key;
static void vis_getin(void)
{
    __asm {
        jsr $ffe4
        sta vis_key
    }
}

// ---------------------------------------------------------------
// vis_add_row — register one channel as displayed, draw its label once.
// No-op once VIS_MAX_ROWS rows are already in use (excess channels are
// simply not displayed -- see ARCHITECTURE.md/the visualiser plan).
// ---------------------------------------------------------------
static void vis_add_row(unsigned char channel, const char *label)
{
    if (vis_row_count >= VIS_MAX_ROWS)
        return;
    vis_channel_row[channel] = vis_row_count;
    cwin_putat_string(&cw, 0, (char)(VIS_ROWS_START + vis_row_count), label, COL_VIS_LABEL);
    vis_row_count++;
}

// ---------------------------------------------------------------
// vis_build_layout — scan hb_songdata.sid_addresses for populated SID
// chips (matches hb_init_sid_image_and_volumes()'s own reading of this
// table) and lay out rows: all 7 UA channels first, then 3 rows per
// populated SID chip in chip order.
// ---------------------------------------------------------------
static void vis_build_layout(void)
{
    unsigned char i, ch;
    char label[6];

    for (i = 0; i < 31; i++)
        vis_channel_row[i] = 0xFF;
    vis_row_count = 0;
    memset(vis_row_level, 0, sizeof(vis_row_level));

    for (ch = 0; ch < HB_UA_CHANNELS; ch++)
    {
        sprintf(label, "UA-%u", ch);
        vis_add_row(ch, label);
    }

    for (i = 0; i < HB_MAX_SIDS; i++)
    {
        unsigned addr = (unsigned)hb_songdata.sid_addresses[i * 2] |
                         ((unsigned)hb_songdata.sid_addresses[i * 2 + 1] << 8);
        unsigned char c;

        if (addr == 0)
            continue;

        for (c = 0; c < 3; c++)
        {
            sprintf(label, "S%u-%u", (unsigned)(i + 1), (unsigned)(c + 1));
            vis_add_row((unsigned char)(7 + i * 3 + c), label);
        }
    }
}

// ---------------------------------------------------------------
// vis_draw_row — redraw one row's bar for its current level (0-63).
// Fixed 3-band colour zones across the bar's own width (green/yellow/red
// at ~60%/85%/100%), not proportional to the current level -- a classic
// VU-meter look where the red zone only lights up near peak.
//
// Uses the _raw putat variants deliberately: SC_REVSPACE/SC_SPACE (from
// defines.h) are already final, literal screen codes, not PETSCII-domain
// source characters -- the non-raw cwin_putat_char() runs its `ch`
// argument through the same runtime PETSCII->screencode conversion used
// for text strings, which silently corrupts an already-final code (e.g.
// SC_REVSPACE $A0 becomes $60, a different glyph entirely -- confirmed by
// reading live screen RAM while the bug was present). screen.c's
// header_line()/screen_header_line() sidesteps the same trap the same
// way, for the same reason.
// ---------------------------------------------------------------
static void vis_draw_row(unsigned char row, unsigned char level)
{
    char y = (char)(VIS_ROWS_START + row);
    unsigned char filled = (unsigned char)(((unsigned)level * VIS_BAR_WIDTH) / 63);
    unsigned char green_end = (unsigned char)((VIS_BAR_WIDTH * 60) / 100);
    unsigned char yellow_end = (unsigned char)((VIS_BAR_WIDTH * 85) / 100);
    unsigned char x;

    for (x = 0; x < VIS_BAR_WIDTH; x++)
    {
        if (x < filled)
        {
            char color = (x < green_end) ? COL_VIS_BAR_LOW
                       : (x < yellow_end) ? COL_VIS_BAR_MID
                       : COL_VIS_BAR_HIGH;
            cwin_putat_char_raw(&cw, (char)(VIS_BAR_X + x), y, SC_REVSPACE, color);
        }
        else
        {
            cwin_putat_char_raw(&cw, (char)(VIS_BAR_X + x), y, SC_SPACE, COL_BACKGROUND);
        }
    }
}

// ---------------------------------------------------------------
// vis_decay_and_draw — once-per-frame update. Reads hb_vis_events[]/
// hb_vis_event_count directly off the live globals; this is inherently
// racy against the tick IRQ updating them concurrently (no synchronization
// -- same accepted tradeoff as this project's existing unsynchronized
// read of hb_ext_out), fine for a visual-only, non-correctness-critical
// display.
// ---------------------------------------------------------------
static void vis_decay_and_draw(void)
{
    unsigned char i, count;

    for (i = 0; i < vis_row_count; i++)
        vis_row_level[i] = (unsigned char)((vis_row_level[i] > 3) ? vis_row_level[i] - 3 : 0);

    // hb_tick() never resets this queue itself (see its own comment on why)
    // -- draining it is this function's job, once per frame. Reset the
    // count back to 0 right after taking our snapshot so new events from
    // ticks between now and the next frame start writing from index 0
    // again, rather than piling up past HB_VIS_MAX_EVENTS and wrapping.
    count = hb_vis_event_count;
    hb_vis_event_count = 0;

    for (i = 0; i < count; i++)
    {
        unsigned char channel = hb_vis_events[i].channel;
        unsigned char row;

        if (channel >= 31)
            continue;
        row = vis_channel_row[channel];
        if (row == 0xFF)
            continue;
        vis_row_level[row] = hb_vis_events[i].velocity;
    }

    for (i = 0; i < vis_row_count; i++)
        vis_draw_row(i, vis_row_level[i]);
}

// ---------------------------------------------------------------
// vis_screen_init — switch to the visualiser's own screen: header,
// per-channel labels (via vis_build_layout()), and the footer key hints.
// ---------------------------------------------------------------
static void vis_screen_init(void)
{
    vic_setmode(VICM_TEXT, (char *)0x0400, (char *)0x1800);
    vic.color_border = COL_BORDER;
    vic.color_back   = COL_BACKGROUND;

    cwin_init(&cw, (char *)0x0400, 0, 0, 40, 25);
    cwin_clear(&cw);

    screen_header_line(0, "heartbeat-demo", COL_HEADER1);
    screen_header_line(1, "Now Playing",    COL_HEADER2);

    vis_build_layout();

    cwin_cursor_move(&cw, 0, (char)(VIS_ROWS_START + VIS_MAX_ROWS + 1));
    screen_hint("SPACE=restart  STOP=silence");
    screen_hint("A-O=play FX 1-15   X=stop FX");
    screen_hint("RETURN=exit to BASIC");
}

// ---------------------------------------------------------------
// visualizer_run — see visualizer.h.
// ---------------------------------------------------------------
void visualizer_run(void)
{
    vis_screen_init();

    for (;;)
    {
        vic_waitFrame();
        vis_decay_and_draw();

        vis_getin();

        if (vis_key == 0x0D)       // RETURN: exit
            break;
        else if (vis_key == 0x20)  // Space: restart music
            hb_init(0, 1);
        else if (vis_key == 0x03)  // Run/Stop: stop all sound
            hb_stop_all();
        else if ((vis_key & 0xF0) == 0x40) // A-O ($41-$4F)
        {
            unsigned char n = (unsigned char)(vis_key & 0x0F);
            if (n != 0) // no sample $00 ('@')
                hb_play_fx(6, n, 0x26); // channel 6, note C-4
        }
        else if (vis_key == 0x58)  // 'X': stop FX channel 6
            hb_stop_fx(6);
    }
}
