#ifndef DETECT_H
#define DETECT_H

// ---------------------------------------------------------------
// Hardware detection for heartbeat-demo
//
// Each function checks one hardware requirement and returns a
// simple result code.  Call in order: UCI → REU → TURBO → AUDIO.
// ---------------------------------------------------------------

// Result codes (generic pass/fail)
#define DETECT_OK   1
#define DETECT_FAIL 0

// REU size result — MB (0 = absent or too small)
extern unsigned char detected_reu_mb;

// Turbo speed class — TURBO_NOT_PRESENT / TURBO_48MHZ / TURBO_64MHZ
extern char detected_turbo_class;

// Ultimate Audio module version byte
extern unsigned char detected_audio_version;

// ---------------------------------------------------------------
// Prototypes
// ---------------------------------------------------------------

char detect_uci(void);
// Poll uii_detect() with up to 10-second timeout.
// Input:  none
// Output: DETECT_OK if UCI registers respond ($DF1D = $C9), DETECT_FAIL otherwise
// Syntax: if (!detect_uci()) { screen_error_exit("No UCI found.", "..."); return 1; }

unsigned char detect_reu(void);
// Write/read test patterns at REU addresses 0 and $F00000.
// Input:  none
// Output: detected size in MB (0 = REU absent or smaller than 16 MB, 16 = 16 MB
//         REU confirmed); also sets detected_reu_mb
// Syntax: unsigned char mb = detect_reu(); if (mb < 16) { ... }

char detect_turbo(void);
// Call turbo_detect() (from turbo.h) which measures CIA1 timer
// loop timing at 1 MHz vs maximum speed.
// Input:  none
// Output: DETECT_OK if turbo registers are present and active, DETECT_FAIL if
//         $D031 == $FF or no speedup measurable; also sets detected_turbo_class
// Syntax: if (detect_turbo()) { ... check detected_turbo_class ... }

char detect_audio(void);
// Call audio_detect() (from audio.h).
// Input:  none
// Output: DETECT_OK if Ultimate Audio module responds, DETECT_FAIL otherwise;
//         also sets detected_audio_version
// Syntax: if (detect_audio()) { ... use detected_audio_version ... }

#pragma compile("detect.c")

#endif
