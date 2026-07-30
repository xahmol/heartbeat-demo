#ifndef VISUALIZER_H
#define VISUALIZER_H

// ---------------------------------------------------------------
// Note visualiser + buttons.s-equivalent test harness screen.
//
// Shown after hardware detection completes and the song has started
// playing. Draws a horizontal VU-meter bar per active channel (7
// Ultimate Audio channels + 3 per populated SID chip), fed from
// hbplayer.h's hb_vis_events[]/hb_vis_event_count queue: a note-on event
// jumps that channel's bar to peak velocity, which then decays each VIC
// frame until the next event. See ARCHITECTURE.md/HEARTBEATPLAYERMANUAL.md
// for the event queue's semantics and channel numbering.
// ---------------------------------------------------------------

void visualizer_run(void);
// Switches to the visualiser's own screen (clears, draws header/labels/
// hint line), then loops once per VIC frame -- decaying and redrawing
// every channel's bar plus plasma/spectroscope/scroller, and polling the
// same buttons.s key bindings the test harness always had (SPACE/RUN-STOP/
// A-O/X), plus 'S' to switch songs -- until RETURN is pressed, at which
// point it returns to the caller.
// Input:  none (reads the live hb_vis_events[]/hb_songdata globals)
// Output: none (blocks until RETURN is pressed)
// Syntax: hb_init(0, 1); visualizer_run(); hb_stop_all();

// Song table shared with main.c (which loads vis_song_files[vis_song_index]
// at startup, index 0, before the visualiser's own 'S' key can switch it).
// Filenames need the raw-ASCII bytes UCI's filesystem protocol expects --
// see visualizer.c's identity-charmap wrapping around the definition.
#define VIS_NUM_SONGS 2
extern char vis_song_files[VIS_NUM_SONGS][24];
extern const char *const vis_song_names[VIS_NUM_SONGS];
extern unsigned char vis_song_index;

#pragma compile("visualizer.c")

#endif
