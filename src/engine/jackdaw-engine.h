#ifndef JACKDAW_ENGINE_H_INCLUDED
#define JACKDAW_ENGINE_H_INCLUDED

#include <jack/jack.h>
#include "project.h"
#include "track.h"

G_BEGIN_DECLS

/*
 * jackdaw-engine: one JACK client, named "jackdaw".
 *
 * Phase-1 registered ports:
 *   audio output: out_1 (L master), out_2 (R master)
 *   metronome:    a dedicated mono click output
 * Per-track capture, MIDI and configurable port counts return in phase 2.
 *
 * Threading contract: every non-RT entry point below is single-caller — the
 * main window's looper thread only (plus jackdaw_engine_init/quit from main()
 * before the window exists / after it quits). Other windows must route engine
 * work through the main window via BMessenger, never call in directly.
 */

/* ---- UI event hook ----
 * JACK notification threads (port registration, graph changes, server
 * shutdown) must never touch UI or emit GObject signals directly. Instead the
 * engine invokes this hook; the UI implements it as a non-blocking
 * BMessenger::SendMessage to the main window, whose handler re-reads engine
 * state. Set the hook BEFORE jackdaw_engine_init so no event is missed. */
typedef enum {
    JACKDAW_ENGINE_EVENT_PORTS_CHANGED = 1,
    JACKDAW_ENGINE_EVENT_CONNECTIONS_CHANGED,
    JACKDAW_ENGINE_EVENT_SHUTDOWN /* JACK server went away; engine is inactive */
} JackDawEngineEvent;

typedef void (*JackDawEngineEventHook)(int event, void *user);
void jackdaw_engine_set_event_hook(JackDawEngineEventHook hook, void *user);

/* Initialise the engine and activate the JACK client.
 * Must be called from the main thread before adding any tracks.
 * Returns FALSE on success, TRUE on failure. */
gboolean jackdaw_engine_init(JackDawProject *project);

/* Deactivate and close the JACK client. Safe to call even if not initialised. */
void jackdaw_engine_quit(void);

/* Returns TRUE if the JACK client is currently active. */
gboolean jackdaw_engine_is_running(void);

/* Returns TRUE while ENGINE_RECORDING flag is set. */
gboolean jackdaw_engine_is_recording(void);

/* Returns TRUE while ENGINE_PLAYING flag is set. */
gboolean jackdaw_engine_is_playing(void);

/* --- Track management --- */
/* Add a track to the engine; allocates track->slot. Returns FALSE on success,
 * TRUE on failure. */
gboolean jackdaw_engine_add_track(JackDawTrack *track);
void jackdaw_engine_remove_track(JackDawTrack *track);

/* --- Transport --- */
void jackdaw_engine_start_playback(void);
void jackdaw_engine_stop_playback(void);
void jackdaw_engine_start_recording(void);
void jackdaw_engine_stop_recording(void);
/* Sound `beats` metronome clicks, then start playback (record=FALSE) or
 * recording (record=TRUE). Returns FALSE if no pre-roll was started (caller
 * starts now). */
gboolean jackdaw_engine_begin_countin(guint beats, gboolean record);
void jackdaw_engine_locate(off_t sample);

/* --- Loop region ---
 * A [start, end) frame span the playhead wraps back over while looping is
 * enabled. The wrap fires only once the playhead has entered the region
 * (block-start in [start, end)); a playhead placed after the region plays
 * straight through. set_loop_range normalises start <= end. All accesses are
 * atomic (written on the window thread, read by the RT callback). */
void jackdaw_engine_set_loop_range(off_t start, off_t end);
void jackdaw_engine_get_loop_range(off_t *start, off_t *end);
void jackdaw_engine_set_loop_enabled(gboolean on);
gboolean jackdaw_engine_get_loop_enabled(void);
gboolean jackdaw_engine_has_loop_region(void); /* TRUE when loop_end > loop_start */

/* --- Record mode ---
 * RECORD_MODE_PUNCH auto-records armed audio tracks over the loop-tab region
 * [loop_start, loop_end) (wired in the recording phase); RECORD_MODE_NORMAL
 * records from the cursor. Stored here so the RT capture path can read it. */
enum { RECORD_MODE_NORMAL = 0, RECORD_MODE_PUNCH = 1 };
void jackdaw_engine_set_record_mode(int mode);
int jackdaw_engine_get_record_mode(void);

/* TRUE while a count-in pre-roll is sounding (transport not yet engaged). */
gboolean jackdaw_engine_is_counting_in(void);

/* Sample rate reported by JACK (valid after jackdaw_engine_init) */
jack_nframes_t jackdaw_engine_get_sample_rate(void);
jack_nframes_t jackdaw_engine_get_buffer_size(void);

/* Monotonically-increasing playback position in samples.
 * Reset by jackdaw_engine_locate(); increments nframes per process cycle
 * while ENGINE_PLAYING is set. Read by main thread for display only. */
off_t jackdaw_engine_get_play_pos(void);

/* Post-master-fader peak levels (master VU). Resets the stored peak on read. */
void jackdaw_engine_get_master_peaks(gfloat *out_L, gfloat *out_R);

/* Cumulative xrun count reported by JACK since engine init (display only). */
guint jackdaw_engine_get_xrun_count(void);

G_END_DECLS

#endif /* JACKDAW_ENGINE_H_INCLUDED */
