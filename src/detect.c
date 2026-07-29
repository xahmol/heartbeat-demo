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
// Workaround for an Oscar64 optimizer regression (toolchain commit 3bbffe9,
// "Improve __memmap storage modifier adherence for REU usage", 2026-06-20,
// and its follow-ups; still present as of 0808a62 / v1.32.272, the version
// installed here): the library's inline reu_count_pages() has its
// volatile-read comparisons of the REU probe byte dead-code-eliminated at
// -O2, so it always returns 0 even when a REU is present. Passing the probe
// byte through this __noinline barrier forces the compiler to materialize
// the value at a real call boundary instead of assuming it away. Same fix
// already applied in UBoot64-v2 (src/main.c). See oscar64manual.md /
// ~/.claude/oscar64.md for the full diagnosis.
// ---------------------------------------------------------------
__noinline char reu_probe_barrier(char v) {
    return v;
}

static int hbdemo_reu_count_pages(void) {
    volatile char c, d;

    c = 0;
    reu_store(0, &c, 1);
    reu_load(0, &d, 1);

    if (reu_probe_barrier(d) == 0) {
        c = 0x47;
        reu_store(0, &c, 1);
        reu_load(0, &d, 1);

        if (reu_probe_barrier(d) == 0x47) {
            for (int i = 1; i < 256; i++) {
                long l = (long)i << 16;
                c = 0x47;
                reu_store(l, &c, 1);
                c = 0x00;
                reu_store(0, &c, 1);

                reu_load(l, &d, 1);
                if (reu_probe_barrier(d) != 0x47)
                    return i;
            }

            return 256;
        }
    }

    return 0;
}

// ---------------------------------------------------------------
// detect_reu
// Uses hbdemo_reu_count_pages() (see workaround above), which returns the
// number of 64 KB pages (256 = 16 MB, 128 = 8 MB, etc.).
// The test is non-destructive enough for startup detection.
// ---------------------------------------------------------------
unsigned char detect_reu(void) {
    int pages = hbdemo_reu_count_pages();

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
