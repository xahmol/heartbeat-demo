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
// Returns DETECT_OK if UCI registers respond ($DF1D = $C9),
// DETECT_FAIL otherwise.

unsigned char detect_reu(void);
// Write/read test patterns at REU addresses 0 and $F00000.
// Returns detected size in MB:
//   0  = REU absent or smaller than 16 MB
//   16 = 16 MB REU confirmed
// Also sets detected_reu_mb.

char detect_turbo(void);
// Call turbo_detect() (from turbo.h) which measures CIA1 timer
// loop timing at 1 MHz vs maximum speed.
// Returns DETECT_OK if turbo registers are present and active,
// DETECT_FAIL if $D031 == $FF or no speedup measurable.
// Also sets detected_turbo_class.

char detect_audio(void);
// Call audio_detect() (from audio.h).
// Returns DETECT_OK if Ultimate Audio module responds.
// Also sets detected_audio_version.

#pragma compile("detect.c")

#endif
