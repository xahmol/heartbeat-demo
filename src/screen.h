#ifndef SCREEN_H
#define SCREEN_H

#include "defines.h"

// All colour (COL_*), screen code (SC_*), and CharWin (cw) definitions
// come from defines.h — do not redefine them here.

// ---------------------------------------------------------------
// Prototypes
// ---------------------------------------------------------------

void screen_init(const char *title);
// Initialise VIC text mode, clear screen, draw two-line header.
// Input:  title — subtitle shown on header line 1
// Output: none
// Syntax: screen_init("Hardware Detection  v1.0.0");

void screen_header_line(char row, const char *text, char color);
// Draw one reversed-video, centered header line at the given row (used by
// screen_init() for its own two-line header; exported so other screens,
// e.g. src/visualizer.c, can draw a matching header without duplicating
// the underlying p2s/reverse-video-bit logic).
// Input:  row   — screen row 0-24 to draw on
//         text  — text to center (source case, converted via petscii.h)
//         color — text color (a COL_*/VCOL_* constant)
// Output: none
// Syntax: screen_header_line(0, "Now Playing", COL_HEADER1);

void screen_result(const char *label, char ok, const char *detail);
// Print one detection result line:
//   "  LABEL : [ OK ] detail"   (COL_OK badge, COL_DETAIL_OK text)
//   "  LABEL : [FAIL] detail"   (COL_FAIL badge, COL_DETAIL_FAIL text)
// Advances the cursor to the next line.
// Input:  label  — short label (e.g. "REU  "), padded to align the ":" column
//         ok     — nonzero for the "[ OK ]" badge, 0 for "[FAIL]"
//         detail — extra text shown after the badge
// Output: none
// Syntax: screen_result("REU  ", 1, "16 MB");

void screen_info(const char *msg);
// Print a plain information line in COL_INFO. Newline appended.
// Input:  msg — text to print
// Output: none
// Syntax: screen_info("Checking turbo mode...");

void screen_hint(const char *msg);
// Print a hint/action line ("  -> msg") in COL_HINT. Newline appended.
// Input:  msg — hint text, keep <=35 chars to fit the 40-col screen
// Output: none
// Syntax: screen_hint("Enable turbo in Ultimate firmware");

void screen_blank_line(void);
// Output one blank line.
// Input:  none
// Output: none
// Syntax: screen_blank_line();

void screen_error_exit(const char *msg, const char *hint);
// Print error in COL_FAIL, optional hint (max 35 chars), then
// "press key to exit to basic." Waits for a keypress, then RETURNS.
// The caller MUST immediately do `return 1` after calling this.
// Input:  msg  — error message
//         hint — optional follow-up hint, or NULL/empty for none
// Output: none (see caller-contract note above)
// Syntax: screen_error_exit("No UCI found.", "F2 > UCI Settings > Enable"); return 1;

void screen_wait_key(const char *msg);
// Print msg + "Press any key.", wait for keypress.
// Input:  msg — prompt text, or NULL/empty for the default "Press any key to continue."
// Output: none
// Syntax: screen_wait_key("Press any key to start playback.");

#pragma compile("screen.c")

#endif
