// heartbeat-demo — hardware detection

#include <c64/cia.h>
#include <c64/reu.h>
#include <petscii.h>
#include <string.h>
#include "defines.h"
#include "detect.h"
#include "audio.h"
#include "turbo.h"
#include "ultimate_common_lib.h"

#pragma code(code)
#pragma data(data)

// ---------------------------------------------------------------
// Exported results
// ---------------------------------------------------------------
unsigned char detected_reu_mb        = 0;
char          detected_turbo_class   = TURBO_NOT_PRESENT;
unsigned char detected_audio_version = 0;

// ---------------------------------------------------------------
// detect_uci
// ---------------------------------------------------------------
char detect_uci(void) {
    // Poll uii_detect() for up to 10 seconds via CIA1 TOD clock.
    // The Ultimate firmware needs time to boot before UCI responds.
    cia1.tods = 0;
    cia1.todt = 0;
    while (!uii_detect() && cia1.tods < 10) {
        ;
    }
    return uii_detect() ? DETECT_OK : DETECT_FAIL;
}

// ---------------------------------------------------------------
// detect_reu
// Uses Oscar64's reu_count_pages() which returns the number of
// 64 KB pages (256 = 16 MB, 128 = 8 MB, etc.).
// The test is non-destructive enough for startup detection.
// ---------------------------------------------------------------
unsigned char detect_reu(void) {
    int pages = reu_count_pages();

    if (pages == 0) {
        detected_reu_mb = 0;
        return 0;
    }

    // 256 pages × 64 KB = 16 MB
    detected_reu_mb = (pages >= 256) ? 16 : (unsigned char)((unsigned)pages / 16);
    return detected_reu_mb;
}

// ---------------------------------------------------------------
// detect_turbo
// ---------------------------------------------------------------
char detect_turbo(void) {
    detected_turbo_class = turbo_detect();
    return (detected_turbo_class != TURBO_NOT_PRESENT) ? DETECT_OK : DETECT_FAIL;
}

// ---------------------------------------------------------------
// detect_audio
// ---------------------------------------------------------------
char detect_audio(void) {
    if (audio_detect()) {
        detected_audio_version = audio_get_version();
        return DETECT_OK;
    }
    detected_audio_version = 0;
    return DETECT_FAIL;
}
