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
// footer instructions), then loops once per VIC frame -- decaying and
// redrawing every channel's bar, and polling the same buttons.s key
// bindings the test harness always had (SPACE/RUN-STOP/A-O/X) -- until
// RETURN is pressed, at which point it returns to the caller.

#pragma compile("visualizer.c")

#endif
