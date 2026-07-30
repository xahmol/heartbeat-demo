// heartbeat-demo — note visualiser + buttons.s-equivalent test harness
// See visualizer.h for the overview.

#include <c64/charwin.h>
#include <c64/vic.h>
#include <c64/memmap.h>
#include <c64/sprites.h>
#include <petscii.h>
#include <stdio.h>
#include <string.h>
#include "defines.h"
#include "screen.h"
#include "hbplayer.h"
#include "visualizer.h"

// ---------------------------------------------------------------
// Song table -- see visualizer.h. main.c loads vis_song_files[0] at
// startup (before the visualiser ever runs); 'S' in visualizer_run() cycles
// to the next entry via vis_switch_song(). Identity charmap override:
// petscii.h's global charmap would otherwise remap these path bytes to the
// wrong PETSCII case for UCI's raw-ASCII filesystem protocol (same
// rationale as hbplayer.c's hb_install_path).
// ---------------------------------------------------------------
#pragma charmap(97, 97, 26)   // a-z -> a-z (identity, overrides petscii.h)
#pragma charmap(65, 65, 26)   // A-Z -> A-Z (identity)
char vis_song_files[VIS_NUM_SONGS][24] = {
    "maniac.reu",
    "Knight Rider Theme.reu",
};
#pragma charmap(97, 65, 26)   // restore petscii.h: a-z -> A-Z
#pragma charmap(65, 97, 26)   // restore petscii.h: A-Z -> a-z

const char *const vis_song_names[VIS_NUM_SONGS] = {
    "Maniac",
    "Knight Rider Theme",
};
unsigned char vis_song_index = 0;

// ---------------------------------------------------------------
// Memory layout — VIC bank 2 ($8000-$BFFF), $A000-$BFFF only
//
// The visualiser needs its own custom charset (smooth fill glyphs) and
// sprite data (scroller font), both of which must live in the SAME VIC
// bank as the screen. Bank 0 (the detection screen's $0400/$1800) is
// fully claimed by this program's own code/data/bss region -- no free
// space there. $A000-$BFFF of bank 2 is explicitly reserved for exactly
// this in main.c's region pragmas (see the extensive comment there for
// why): the earlier assumption that $8000-$8FFF/$A000-$AFFF were "free"
// purely because a BSSEnd snapshot didn't reach that far was WRONG and
// caused a real, confirmed bug (hb_sids -- live SID channel state --
// landed at $8A6A and got clobbered by the charset copy, killing SID
// playback). Never place anything here without a corresponding pragma
// region carve-out in main.c that the linker itself enforces.
//
// $9000-$9FFF is the hardware character-ROM-shadow range (VIC always
// fetches character/bitmap DATA from the real char ROM there, regardless
// of bank, when bank 0 or 2 is selected) -- staying entirely at $A000+
// keeps well clear of it.
//
//   $A000-$A3E7  screen memory (1000 bytes)
//   $A800-$AFFF  custom charset (2048 bytes = 256 chars x 8 bytes)
//   $B000-$BFFF  sprite font (64 sprites x 64 bytes, 63 real + 1 pad byte
//                each so sprite-pointer arithmetic is a plain glyph index)
// ---------------------------------------------------------------
#define VIS_BANK2_SCREEN   ((char *)0xA000)
#define VIS_BANK2_CHARSET  ((char *)0xA800)
#define VIS_BANK2_SPRITES  ((char *)0xB000)
// Sprite pointer bytes are offsets from the START OF THE CURRENT 16KB VIC
// BANK ($8000 for bank 2) -- NOT from the screen matrix's own address
// ($A000). Using the screen address here was the actual bug behind sprites
// showing garbage: it silently pointed 128 blocks short of the real sprite
// data, into charset/screen memory instead.
#define VIS_SPRITE_PTR_BASE ((unsigned char)((0xB000 - 0x8000) / 64)) // 192

// ---------------------------------------------------------------
// Layout
// ---------------------------------------------------------------
#define VIS_ROWS_START   3    // first channel-bar row (after the 2-line header + 1 blank)
// Two side-by-side columns of channel bars instead of one: 31 possible
// channels (7 UA + up to 24 SID, HB_MAX_SIDS*3) don't fit in a single
// 20-odd-row column, but DO fit in 2 columns of 16 rows (32 slots >= 31),
// so every channel a song ever populates can be shown at once. Each column
// is a 20-char-wide half of the screen (label + bar), same fill-glyph/
// green-yellow-red convention as before, just narrower per bar.
#define VIS_NUM_COLS      2
#define VIS_ROWS_PER_COL  16
#define VIS_TOTAL_SLOTS   (VIS_NUM_COLS * VIS_ROWS_PER_COL)   // 32
#define VIS_LABEL_WIDTH   4    // "UA-6" / "S8-3" -- always exactly 4 chars
#define VIS_COL_WIDTH     (40 / VIS_NUM_COLS)                       // 20
#define VIS_COL_BAR_X(col)  ((col) * VIS_COL_WIDTH + VIS_LABEL_WIDTH + 1)
#define VIS_COL_BAR_WIDTH   (VIS_COL_WIDTH - VIS_LABEL_WIDTH - 1)   // 15

// The spectroscope (below the two bar columns) still spans the FULL screen
// width as one unsplit display -- VIS_BAR_X/VIS_BAR_WIDTH name that span
// specifically (distinct from the per-column VIS_COL_BAR_* above).
#define VIS_SPEC_ROWS    3
#define VIS_SPEC_Y0      (VIS_ROWS_START + VIS_ROWS_PER_COL)
#define VIS_BAR_X        (VIS_LABEL_WIDTH + 1)
#define VIS_BAR_WIDTH     (40 - VIS_BAR_X)

// Fill-glyph screen codes: 0x60-0x66. Originally chosen as 0x81-0x87
// (reverse-video lowercase a-g), which turned out to collide for real --
// the header ("Heartbeat Tracker Player Demo") is rendered in reverse
// video and contains several of those exact letters (confirmed via a live
// memory read of the copied charset at $8800: codes 0x01-0x07 really are
// lowercase a-g, so 0x81-0x87 are their reverse-video forms, several of
// which appear in the header text). Codes 0x60-0x7F are a genuinely safer
// choice: a live read of that range (same charset copy) confirms they're
// real PETSCII graphics/line-drawing glyphs (checkerboards, blocks,
// corners) in this ROM page, not letters/digits/symbols at all -- this
// project never prints graphics characters as text, so this range can't
// collide with anything we ever display, regardless of what header/label/
// scroll text changes in the future.
#define VIS_FILL_BASE 0x60

// vis_fill_pattern[N] = the 8x8 glyph bitmap (same byte repeated for all 8
// rows -- a solid vertical slice, not a per-row shape) for "N eighths
// filled, left-to-right" (N=1..7). N=0 (empty) and N=8 (full) already
// exist as SC_SPACE/SC_REVSPACE and need no custom glyph.
static const unsigned char vis_fill_pattern[8] = { 0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE };

// vis_channel_row[hb_vis channel number 0-30] -> SLOT index 0..VIS_TOTAL_SLOTS-1
// (col = slot / VIS_ROWS_PER_COL, row = slot % VIS_ROWS_PER_COL), or 0xFF if
// that channel isn't displayed. With VIS_TOTAL_SLOTS=32 >= 31 possible
// channels, 0xFF in practice never happens any more -- kept as a defensive
// cap, not a real limit.
static unsigned char vis_channel_row[31];
static unsigned char vis_row_level[VIS_TOTAL_SLOTS];
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
// vis_add_row — register one channel as displayed, draw its label once, in
// the next free slot (column-major: fills column 0's 16 rows, then column
// 1's). No-op once all VIS_TOTAL_SLOTS are in use (in practice never
// happens -- 32 slots cover all 31 possible channels).
// ---------------------------------------------------------------
static void vis_add_row(unsigned char channel, const char *label)
{
    unsigned char slot, col, row;

    if (vis_row_count >= VIS_TOTAL_SLOTS)
        return;
    slot = vis_row_count;
    col = (unsigned char)(slot / VIS_ROWS_PER_COL);
    row = (unsigned char)(slot % VIS_ROWS_PER_COL);
    vis_channel_row[channel] = slot;
    cwin_putat_string(&cw, (char)(col * VIS_COL_WIDTH), (char)(VIS_ROWS_START + row), label, COL_VIS_LABEL);
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
static void vis_draw_row(unsigned char slot, unsigned char level)
{
    unsigned char col = (unsigned char)(slot / VIS_ROWS_PER_COL);
    unsigned char row = (unsigned char)(slot % VIS_ROWS_PER_COL);
    unsigned char bar_x0 = (unsigned char)VIS_COL_BAR_X(col);
    char y = (char)(VIS_ROWS_START + row);
    // Eighth-of-a-character resolution: total_eighths counts how many 1/8
    // slices are filled across the whole bar; full_chars/remainder split
    // that into whole solid characters plus one fractional boundary glyph.
    unsigned total_eighths = ((unsigned)level * VIS_COL_BAR_WIDTH * 8) / 63;
    unsigned char full_chars = (unsigned char)(total_eighths / 8);
    unsigned char remainder = (unsigned char)(total_eighths % 8);
    unsigned char green_end = (unsigned char)((VIS_COL_BAR_WIDTH * 60) / 100);
    unsigned char yellow_end = (unsigned char)((VIS_COL_BAR_WIDTH * 85) / 100);
    unsigned char x;

    for (x = 0; x < VIS_COL_BAR_WIDTH; x++)
    {
        char color = (x < green_end) ? COL_VIS_BAR_LOW
                   : (x < yellow_end) ? COL_VIS_BAR_MID
                   : COL_VIS_BAR_HIGH;

        if (x < full_chars)
            cwin_putat_char_raw(&cw, (char)(bar_x0 + x), y, SC_REVSPACE, color);
        else if (x == full_chars && remainder > 0)
            cwin_putat_char_raw(&cw, (char)(bar_x0 + x), y, (char)(VIS_FILL_BASE + remainder - 1), color);
        // else: deliberately left untouched -- the unfilled tail of the bar
        // shows whatever vis_draw_plasma() painted there earlier this same
        // frame (plasma runs behind the bars; see the ordering note in
        // visualizer_run()), so the bar visually "eats into" the plasma as
        // its level rises instead of masking it with a flat background fill.
    }
}

// ---------------------------------------------------------------
// vis_draw_plasma — 2-sine colour-only plasma (solid blocks, colour
// carries the brightness -- no per-pixel character variation). Runs behind
// the VU bars: repaints BOTH columns' full bar-width span on EVERY row,
// unconditionally, regardless of whether that slot currently holds a
// channel. This unconditional repaint is what makes bars "clear" at all --
// vis_draw_row() only draws the bar's OWN filled portion each frame and
// deliberately leaves the unfilled tail untouched (see its comment), so
// when a level drops, nothing but a freshly-redrawn plasma cell erases the
// previous frame's longer bar. Skipping plasma for "active" slots (an
// earlier version of this function did, as a mistaken optimization) breaks
// exactly that erase for every populated row, which is nearly every row
// once a song fills both columns. Label columns are never touched, so
// per-channel labels drawn once at layout time need no redrawing. Same
// general technique as UltimateDemo2026/src/scroller.c's draw_plasma(), but
// deliberately different phase multipliers AND a different palette (cool
// blue/purple/white here vs. that demo's warm red-to-blue rainbow) so this
// doesn't look like a reskin of it.
// ---------------------------------------------------------------
static const unsigned char vis_psin[64] = {
    4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4,
    4, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3
};
static const unsigned char vis_pcolor[8] = {
    VCOL_BLACK, VCOL_BLUE, VCOL_PURPLE, VCOL_BLUE,
    VCOL_LT_BLUE, VCOL_CYAN, VCOL_LT_BLUE, VCOL_WHITE
};
static unsigned char vis_plasma_phase;

static void vis_draw_plasma_span(unsigned char x0, unsigned char width, char y)
{
    unsigned char i;
    for (i = 0; i < width; i++)
    {
        unsigned char x = (unsigned char)(x0 + i);
        unsigned char v = (unsigned char)(
            (vis_psin[((unsigned char)(x * 7u) + vis_plasma_phase) & 63u] +
             vis_psin[((unsigned char)(y * 11u) + (unsigned char)(vis_plasma_phase * 3u)) & 63u]) >> 1u);
        cwin_putat_char_raw(&cw, (char)x, y, SC_REVSPACE, vis_pcolor[v]);
    }
}

static void vis_draw_plasma(void)
{
    unsigned char row, col;
    for (row = 0; row < VIS_ROWS_PER_COL; row++)
    {
        char y = (char)(VIS_ROWS_START + row);
        for (col = 0; col < VIS_NUM_COLS; col++)
            vis_draw_plasma_span((unsigned char)VIS_COL_BAR_X(col), VIS_COL_BAR_WIDTH, y);
    }
    vis_plasma_phase++;
}

// ---------------------------------------------------------------
// vis_draw_spectroscope — stylized pitch histogram, NOT a real FFT (no
// audio sampling exists to analyze): buckets each hb_vis_events[] note into
// SPEC_BUCKETS bins (one per bar-area column, note/3 so the typical ~0-95
// note range spans the full width) and shows bucket velocity as up to
// VIS_SPEC_ROWS stacked rows of solid colour (bottom row lights at any
// level>0, each row above needs a proportionally higher level -- row j from
// the top needs level > (VIS_SPEC_ROWS-1-j)*63/VIS_SPEC_ROWS), same 3-tier
// green/yellow/red convention and peak-then-decay behaviour as the VU bars
// (see vis_draw_row()). Genuinely reacts to whatever's playing since it
// reads the same live event data.
// ---------------------------------------------------------------
#define SPEC_BUCKETS VIS_BAR_WIDTH
static unsigned char spec_level[SPEC_BUCKETS];

static void vis_draw_spectroscope(void)
{
    unsigned char b, r;
    for (b = 0; b < SPEC_BUCKETS; b++)
    {
        unsigned char level = spec_level[b];
        char color = (level == 0)   ? COL_BACKGROUND
                   : (level < 32)   ? COL_VIS_BAR_LOW
                   : (level < 52)   ? COL_VIS_BAR_MID
                   :                  COL_VIS_BAR_HIGH;
        char x = (char)(VIS_BAR_X + b);

        for (r = 0; r < VIS_SPEC_ROWS; r++)
        {
            unsigned char threshold = (unsigned char)(((VIS_SPEC_ROWS - 1 - r) * 63) / VIS_SPEC_ROWS);
            char y = (char)(VIS_SPEC_Y0 + r);
            cwin_putat_char_raw(&cw, x, y, level > threshold ? SC_REVSPACE : SC_SPACE, color);
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
    for (i = 0; i < SPEC_BUCKETS; i++)
        spec_level[i] = (unsigned char)((spec_level[i] > 3) ? spec_level[i] - 3 : 0);

    // hb_tick() never resets this queue itself (see its own comment on why)
    // -- draining it is this function's job, once per frame. Reset the
    // count back to 0 right after taking our snapshot so new events from
    // ticks between now and the next frame start writing from index 0
    // again, rather than piling up past HB_VIS_MAX_EVENTS and wrapping.
    // Both the per-channel VU levels and the spectroscope's per-bucket
    // levels are fed from this SAME drain pass -- the queue can only be
    // consumed once per frame, so both displays must update together here.
    count = hb_vis_event_count;
    hb_vis_event_count = 0;

    for (i = 0; i < count; i++)
    {
        unsigned char channel = hb_vis_events[i].channel;
        unsigned char note = hb_vis_events[i].note;
        unsigned char row, bucket;

        if (channel < 31)
        {
            row = vis_channel_row[channel];
            if (row != 0xFF)
                vis_row_level[row] = hb_vis_events[i].velocity;
        }

        bucket = (unsigned char)(note / 3);
        if (bucket >= SPEC_BUCKETS)
            bucket = SPEC_BUCKETS - 1;
        spec_level[bucket] = hb_vis_events[i].velocity;
    }

    for (i = 0; i < vis_row_count; i++)
        vis_draw_row(i, vis_row_level[i]);
    vis_draw_spectroscope();
}

// ---------------------------------------------------------------
// vis_scroll — 8-sprite scrolltext, no multiplexing (see the "wow factor"
// plan's analysis of why true same-line multiplexing was ruled out: it
// would need hand-written raster IRQ code competing with hb_irq's own IRQ
// vector, too much risk for this pass). All 8 hardware sprites are reused
// as a single 8-character-wide window: the message advances through it one
// pixel at a time, and whenever a whole 24px-wide sprite cell scrolls past,
// every sprite's image is reassigned to the next message character in one
// step (the classic sprite-scrolltext technique) -- so the 8 physical
// sprites present what looks like unlimited scrolling text.
//
// Sprite font: "Sprite Font" by hedning, from the demo *World in Progress*
// (1st place Mixed category, Syntax Society Summerparty 2012),
// https://c64gfx.com/image/9998103 -- see README.md credits.
// ---------------------------------------------------------------
static const unsigned char vis_spritefont[4096] = {
    #embed "../assets/spritefont.bin"
};

// vis_font_index — map a plain-ASCII scrolltext character to hedning's font's
// glyph index. The font follows standard PETSCII screen-code ordering
// (confirmed by inspecting the extracted glyphs): 0='@', 1-26='A'-'Z',
// 32=space, 45='-', 46='.', 48-57='0'-'9'. Anything else falls back to space
// rather than showing a wrong/garbage glyph.
static unsigned char vis_font_index(char ch)
{
    if (ch == ' ')  return 32;
    if (ch >= 'A' && ch <= 'Z') return (unsigned char)(ch - 'A' + 1);
    if (ch >= '0' && ch <= '9') return (unsigned char)(ch - '0' + 48);
    if (ch == '-')  return 45;
    if (ch == '.')  return 46;
    if (ch == '@')  return 0;
    return 32;
}

static const char vis_scroll_msg[] =
    "        HEARTBEAT TRACKER PLAYER DEMO -"
    " HEARTBEAT SOUNDTRACKER IS A MUSIC TRACKER FOR THE ULTIMATE 64"
    " USING SID AND ULTIMATE AUDIO CHANNELS TOGETHER -"
    " THIS DEMO PLAYS MANIAC BY MICHAEL SEMBELLO ARRANGED BY XANDER MOL"
    " AND THE KNIGHT RIDER THEME ARRANGED BY ALEKSI EEBEN -"
    " PLAYER PORT AND CODE BY CLAUDE -"
    " SPRITE FONT BY HEDNING FROM WORLD IN PROGRESS -"
    " THANKS FOR WATCHING -        ";
#define VIS_SCROLL_MSG_LEN (sizeof(vis_scroll_msg) - 1)

#define VIS_SCROLL_NUM_SPR 8
#define VIS_SCROLL_CHAR_W  24   // hedning's font's native sprite width, in pixels -- governs the
                                // scroll-wrap pixel granularity (when to advance to the next message
                                // character), independent of the on-screen spacing below.
// On-screen spacing between the 8 simultaneous sprites: wide enough that all
// 8 spread across the FULL 40-column screen width, not just the bar area --
// 24 (leftmost visible sprite X) to 24+7*40+24=328, just inside the 344 max.
#define VIS_SCROLL_PITCH   40
#define VIS_SCROLL_BASE_X  24   // leftmost visible sprite X coordinate (standard C64 constant)

// Y bounds: standard sprite-coordinate offsets (Y=50 == top of the text
// screen, 8px/row) clamped so the 21px-tall sprite stays within the VU
// bar/plasma area (rows VIS_ROWS_START..VIS_ROWS_START+VIS_ROWS_PER_COL-1)
// and never strays into the spectroscope band below it.
#define VIS_SPR_Y_MIN (50 + VIS_ROWS_START * 8)
#define VIS_SPR_Y_MAX (50 + (VIS_ROWS_START + VIS_ROWS_PER_COL) * 8 - 21)

// Colour gradient across the 8 sprite slots: warm brown/orange/pink/grey,
// deliberately disjoint from the plasma's cool blue/purple/cyan/white
// palette and the VU bars'/spectroscope's green/yellow/red, so the scroller
// never blends into whatever's animating behind or around it.
static const unsigned char vis_scroll_color[VIS_SCROLL_NUM_SPR] = {
    VCOL_BROWN, VCOL_ORANGE, VCOL_LT_RED, VCOL_LT_GREY,
    VCOL_LT_GREY, VCOL_LT_RED, VCOL_ORANGE, VCOL_BROWN
};

static unsigned int vis_scroll_msg_pos; // wider than unsigned char: the message is well over 255 chars
static unsigned char vis_scroll_pixel;
static int           vis_scroll_base_y;
static signed char vis_scroll_dir;
static unsigned char vis_scroll_ripple_phase;

static void vis_scroll_init(void)
{
    spr_init(VIS_BANK2_SCREEN);
    memcpy(VIS_BANK2_SPRITES, vis_spritefont, sizeof(vis_spritefont));

    vis_scroll_msg_pos = 0;
    vis_scroll_pixel = 0;
    vis_scroll_base_y = VIS_SPR_Y_MIN;
    vis_scroll_dir = 1;
    vis_scroll_ripple_phase = 0;
}

// vis_scroll_update — once-per-frame: advance the message by one pixel
// (wrapping to the next character every VIS_SCROLL_CHAR_W pixels), bounce
// the whole window's baseline Y smoothly across the full bar-area height,
// and lay a per-letter sine ripple (reusing vis_psin, the same table
// vis_draw_plasma() uses) on top of that baseline -- together giving both
// halves of the requested look: an overall vertical bounce plus the
// classic letter-by-letter "sine scroller" undulation.
static void vis_scroll_update(void)
{
    unsigned char i;

    vis_scroll_pixel++;
    if (vis_scroll_pixel >= VIS_SCROLL_CHAR_W)
    {
        vis_scroll_pixel = 0;
        vis_scroll_msg_pos++;
        if (vis_scroll_msg_pos >= VIS_SCROLL_MSG_LEN)
            vis_scroll_msg_pos = 0;
    }

    vis_scroll_base_y += vis_scroll_dir;
    if (vis_scroll_base_y >= VIS_SPR_Y_MAX)
    {
        vis_scroll_base_y = VIS_SPR_Y_MAX;
        vis_scroll_dir = -1;
    }
    else if (vis_scroll_base_y <= VIS_SPR_Y_MIN)
    {
        vis_scroll_base_y = VIS_SPR_Y_MIN;
        vis_scroll_dir = 1;
    }
    vis_scroll_ripple_phase++;

    for (i = 0; i < VIS_SCROLL_NUM_SPR; i++)
    {
        unsigned int msg_i = (unsigned int)(vis_scroll_msg_pos + i);
        char ch;
        unsigned char glyph;
        int x;
        signed char ripple;
        int y;
        unsigned char color_idx;

        while (msg_i >= VIS_SCROLL_MSG_LEN)
            msg_i -= VIS_SCROLL_MSG_LEN;
        ch = vis_scroll_msg[msg_i];
        glyph = vis_font_index(ch);

        x = VIS_SCROLL_BASE_X + i * VIS_SCROLL_PITCH - vis_scroll_pixel;
        ripple = (signed char)((vis_psin[(unsigned char)(vis_scroll_ripple_phase + i * 8) & 63] - 4) * 2);
        y = vis_scroll_base_y + ripple;

        // Colour keyed by (vis_scroll_msg_pos + i), NOT by slot i alone: for
        // any one letter, slot i decreases by exactly 1 every time
        // vis_scroll_msg_pos increases by 1 (it shifts one slot left), so
        // this sum is CONSTANT for that letter's whole time on screen -- it
        // only changes when a new letter is revealed at the rightmost slot.
        // Colour was previously keyed by i alone, which meant every letter's
        // colour changed abruptly each time it moved to the next slot.
        color_idx = (unsigned char)((vis_scroll_msg_pos + i) & (VIS_SCROLL_NUM_SPR - 1));

        spr_set((char)i, true, x, (char)y, (char)(VIS_SPRITE_PTR_BASE + glyph),
                vis_scroll_color[color_idx], false, false, false);
    }
}

// ---------------------------------------------------------------
// vis_setup_charset — copy the KERNAL character ROM's lowercase+uppercase
// set (the same one screen.c/vic_setmode's $1800 argument uses on bank 0,
// via the hardware char-ROM shadow) into VIS_BANK2_CHARSET, unmodified
// for now (custom fill glyphs are added in a later, separately-verified
// step). This is a genuine critical section: MMAP_CHAR_ROM disables I/O
// entirely, so if hb_tick's interrupt fired mid-copy and tried to touch
// real hardware registers (SID, CIA ack, etc.) it would hit character ROM
// data instead -- SEI/CLI around the whole switch+copy+restore sequence
// makes that impossible. One-time startup cost only (not per-frame), so
// the brief IRQ-disabled window is negligible even at 1 MHz.
// ---------------------------------------------------------------
static void vis_setup_charset(void)
{
    unsigned char lvl, row;

    __asm { sei }
    mmap_set(MMAP_CHAR_ROM);
    memcpy(VIS_BANK2_CHARSET, (char *)0xD800, 2048); // lowercase+uppercase set (2nd half of char ROM)
    mmap_set(MMAP_NO_BASIC);
    __asm { cli }

    for (lvl = 1; lvl <= 7; lvl++)
    {
        unsigned char *glyph = (unsigned char *)VIS_BANK2_CHARSET + (VIS_FILL_BASE + lvl - 1) * 8;
        for (row = 0; row < 8; row++)
            glyph[row] = vis_fill_pattern[lvl];
    }
}

// ---------------------------------------------------------------
// vis_build_subtitle — "Now Playing: <song name>", bounded to `maxlen`
// characters (plus the null terminator `out` must have room for). Built
// char-by-char rather than sprintf+strlen-after-the-fact so a hypothetical
// long song name can never overflow `out` -- screen_header_line()'s own
// centering math assumes its input is already <=40 chars and does not
// itself guard against a longer string.
// ---------------------------------------------------------------
static void vis_build_subtitle(char *out, char maxlen)
{
    static const char prefix[] = "Now Playing: ";
    const char *name = vis_song_names[vis_song_index];
    char i = 0, j;
    while (prefix[i] && i < maxlen)
    {
        out[(unsigned char)i] = prefix[(unsigned char)i];
        i++;
    }
    j = 0;
    while (name[(unsigned char)j] && i < maxlen)
        out[(unsigned char)i++] = name[(unsigned char)j++];
    out[(unsigned char)i] = 0;
}

// ---------------------------------------------------------------
// vis_draw_static_screen — (re)draws everything that ISN'T redrawn every
// frame: header/subtitle, channel labels, and the control hint line. Called
// once at startup and again after a song switch, since a different song
// can populate a different set of channels (stale labels from the previous
// song must be cleared, not just overwritten in place).
// ---------------------------------------------------------------
static void vis_draw_static_screen(void)
{
    char subtitle[41];

    cwin_clear(&cw);

    screen_header_line(0, "Heartbeat Tracker Player Demo", COL_HEADER1);
    vis_build_subtitle(subtitle, 40);
    screen_header_line(1, subtitle, COL_HEADER2);

    vis_build_layout();

    // Single control-hint line -- kept to <=40 chars deliberately measured,
    // not screen_hint()'s "  -> " prefix (which would push it over 40 here).
    cwin_cursor_move(&cw, 0, (char)(VIS_SPEC_Y0 + VIS_SPEC_ROWS));
    cwin_put_string(&cw, "S=switch song SPACE=restart RETURN=exit", COL_HINT);
}

// ---------------------------------------------------------------
// vis_switch_song — 'S' key: stop playback, load the next song in
// vis_song_files[] into REU, restart, and redraw the static screen (the new
// song may populate a different set of channels). On load failure, leaves
// the previous song's index/state alone rather than restarting with
// (now-overwritten, partially-loaded) garbage song data.
// ---------------------------------------------------------------
static void vis_switch_song(void)
{
    unsigned char next_index = (unsigned char)((vis_song_index + 1) % VIS_NUM_SONGS);
    char loaded;

    hb_stop_all();
    screen_header_line(1, "Loading song...", COL_HEADER2);

    loaded = hb_load(vis_song_files[next_index], HB_SONG_REU_BASE);
    if (loaded)
    {
        vis_song_index = next_index;
        hb_init(0, 1);
    }
    vis_draw_static_screen(); // redraw regardless -- clears the "Loading..." line either way
}

// ---------------------------------------------------------------
// vis_screen_init — switch to the visualiser's own screen: one-time charset/
// sprite setup, then the header/labels/hint line via vis_draw_static_screen().
// Uses VIC bank 2 (see the memory layout comment above), not bank 0 --
// this is a DIFFERENT bank than screen.c's detection screen, which is
// fine: only one bank is ever "active" at a time, and the whole screen is
// torn down and rebuilt on this transition anyway.
// ---------------------------------------------------------------
static void vis_screen_init(void)
{
    vis_setup_charset();

    vic_setmode(VICM_TEXT, VIS_BANK2_SCREEN, VIS_BANK2_CHARSET);
    vic.color_border = COL_BORDER;
    vic.color_back   = COL_BACKGROUND;

    cwin_init(&cw, VIS_BANK2_SCREEN, 0, 0, 40, 25);

    vis_draw_static_screen();
    vis_scroll_init();
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
        // Sprite register writes go FIRST, right after vblank: vis_draw_plasma()
        // and vis_decay_and_draw() below are a variable-cost few hundred
        // character-cell pokes (more with more active channels), and hb_tick's
        // own ~195Hz interrupt can land mid-draw too -- if sprite updates ran
        // after that variable-cost work instead, they'd land at an
        // inconsistent point in the frame from one frame to the next
        // (sometimes still near vblank, sometimes well into the visible
        // scan), which reads as jerky sprite motion. Running it first keeps
        // the sprite writes' timing consistent regardless of how much bar/
        // plasma work that frame needs.
        vis_scroll_update();
        vis_draw_plasma();      // must run before vis_decay_and_draw(): bars draw on top of it
        vis_decay_and_draw();

        vis_getin();

        if (vis_key == 0x0D)       // RETURN: exit
            break;
        else if (vis_key == 0x20)  // Space: restart music
            hb_init(0, 1);
        else if (vis_key == 0x03)  // Run/Stop: stop all sound
            hb_stop_all();
        else if (vis_key == 0x53)  // 'S': switch song
            vis_switch_song();
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
