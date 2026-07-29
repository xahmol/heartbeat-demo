#ifndef DEFINES_H_
#define DEFINES_H_

#include "c64/vic.h"
#include "c64/charwin.h"

// ---------------------------------------------------------------
// Screen codes
// ---------------------------------------------------------------
#define SC_SPACE    0x20   // space character screen code
#define SC_REVSPACE 0xA0   // reversed space (solid block)

// ---------------------------------------------------------------
// Utility macro
// ---------------------------------------------------------------
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

// ---------------------------------------------------------------
// Application identity
// ---------------------------------------------------------------
#define APP_NAME    "heartbeat-demo"

// ---------------------------------------------------------------
// Colour palette for the demo UI
// (COL_* used by screen.h / screen.c)
// ---------------------------------------------------------------
#define COL_BACKGROUND   VCOL_BLACK
#define COL_BORDER       VCOL_BLACK
#define COL_HEADER1      VCOL_GREEN
#define COL_HEADER2      VCOL_LT_GREEN
#define COL_LABEL        VCOL_WHITE
#define COL_OK           VCOL_GREEN
#define COL_FAIL         VCOL_RED
#define COL_DETAIL_OK    VCOL_CYAN
#define COL_DETAIL_FAIL  VCOL_YELLOW
#define COL_INFO         VCOL_LT_GREY
#define COL_HINT         VCOL_YELLOW
#define COL_SUCCESS      VCOL_LT_GREEN
#define COL_KEY          VCOL_WHITE

// ---------------------------------------------------------------
// Global CharWin — declared in screen.c, used everywhere
// ---------------------------------------------------------------
extern CharWin cw;

#endif
