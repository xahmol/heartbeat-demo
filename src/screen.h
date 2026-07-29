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
// title: subtitle shown on header line 1.

void screen_result(const char *label, char ok, const char *detail);
// Print one detection result line:
//   "  LABEL : [ OK ] detail"   (COL_OK badge, COL_DETAIL_OK text)
//   "  LABEL : [FAIL] detail"   (COL_FAIL badge, COL_DETAIL_FAIL text)
// Advances the cursor to the next line.

void screen_info(const char *msg);
// Print a plain information line in COL_INFO. Newline appended.

void screen_hint(const char *msg);
// Print a hint/action line ("  -> msg") in COL_HINT. Newline appended.

void screen_blank_line(void);
// Output one blank line.

void screen_error_exit(const char *msg, const char *hint);
// Print error in COL_FAIL, optional hint (max 35 chars), then
// "press key to exit to basic." Waits for a keypress, then RETURNS.
// The caller MUST immediately do `return 1` after calling this.

void screen_wait_key(const char *msg);
// Print msg + "Press any key.", wait for keypress.

#pragma compile("screen.c")

#endif
