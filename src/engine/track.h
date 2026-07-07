#ifndef TRACK_H_INCLUDED
#define TRACK_H_INCLUDED

#include <glib-object.h>

G_BEGIN_DECLS

/* Track kind: an audio track streams AudioClip regions; an instrument track
 * sequences MidiRegions into its first FX-chain plugin (the instrument).
 * Phase 1 carries the kind only; clip/MIDI/FX members return in phase 2. */
typedef enum { JACKDAW_TRACK_AUDIO = 0, JACKDAW_TRACK_INSTRUMENT } JackDawTrackKind;

#define JACKDAW_TYPE_TRACK (jackdaw_track_get_type())
#define JACKDAW_TRACK(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, JACKDAW_TYPE_TRACK, JackDawTrack))
#define JACKDAW_IS_TRACK(obj) (G_TYPE_CHECK_INSTANCE_TYPE(obj, JACKDAW_TYPE_TRACK))

/* State flag bits — written by main thread, read by RT callback */
#define TRACK_ARMED (1 << 0)
#define TRACK_MUTED (1 << 1)
#define TRACK_SOLOED (1 << 2)

/* Max tracks the engine supports */
#define JACKDAW_MAX_TRACKS 64

typedef struct _JackDawTrack JackDawTrack;
typedef struct _JackDawTrackClass JackDawTrackClass;

struct _JackDawTrack {
    GObject parent_instance;

    gchar *name;
    guint slot;            /* index in engine track slot array */
    JackDawTrackKind kind; /* audio (default) or instrument */

    /* Input routing — indices into engine port arrays; -1 = none. */
    gint audio_in_idx;
    gint midi_in_idx;

    /* External JACK ports connected to this track's capture ports (main-thread
     * only; NULL = no jackdaw-made connection). audio_src_port is the left/mono
     * source; audio_src_port_r is the right source (NULL when mono). */
    gchar *audio_src_port;
    gchar *audio_src_port_r;
    gchar *midi_src_port;  /* external MIDI source (instrument tracks); NULL = none */
    gboolean stereo_input; /* TRUE = the track captures a stereo pair */

    /* RT-safe state: written atomically by main thread */
    volatile gint32 state_flags;
    volatile gfloat volume; /* EFFECTIVE gain (= trim * fader), RT reads this */
    volatile gfloat trim;   /* input trim gain (track strip dial) */
    volatile gfloat fader;  /* channel fader gain (mixer fader) */
    volatile gfloat pan;    /* -1.0 (L) … 0.0 (C) … 1.0 (R) */

    /* Peak metering: written by RT callback, read by main thread */
    volatile gfloat peak_L;
    volatile gfloat peak_R;
};

struct _JackDawTrackClass {
    GObjectClass parent_class;

    void (*state_changed)(JackDawTrack *track);
    void (*routing_changed)(JackDawTrack *track);
};

GType jackdaw_track_get_type(void);

/* Constructor. Phase 2 restores the AudioClip take-ownership parameter the
 * Linux original has here. */
JackDawTrack *jackdaw_track_new(const gchar *name);

/* Accessors (main-thread safe) */
const gchar *jackdaw_track_get_name(JackDawTrack *t);
void jackdaw_track_set_name(JackDawTrack *t, const gchar *name);

/* ---- Track kind (main thread) ---- */
JackDawTrackKind jackdaw_track_get_kind(JackDawTrack *t);
void jackdaw_track_set_kind(JackDawTrack *t, JackDawTrackKind kind);
gboolean jackdaw_track_is_instrument(JackDawTrack *t);

/* State flag helpers — use g_atomic_int_or/and for thread safety */
void jackdaw_track_set_armed(JackDawTrack *t, gboolean armed);
void jackdaw_track_set_muted(JackDawTrack *t, gboolean muted);
void jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed);
gboolean jackdaw_track_is_armed(JackDawTrack *t);
gboolean jackdaw_track_is_muted(JackDawTrack *t);
gboolean jackdaw_track_is_soloed(JackDawTrack *t);

/* Volume/pan — stored as volatile float, main thread writes, RT reads */
/* Effective gain = trim * fader. set_volume() is legacy (sets the fader stage
 * with trim left at unity); get_volume() returns the effective product. */
void jackdaw_track_set_volume(JackDawTrack *t, gfloat vol);
gfloat jackdaw_track_get_volume(JackDawTrack *t);

/* Two independent gain stages (gain staging): the track-strip dial drives the
 * trim; the mixer fader drives the fader. Both fold into the effective volume. */
void jackdaw_track_set_trim(JackDawTrack *t, gfloat trim);
gfloat jackdaw_track_get_trim(JackDawTrack *t);
void jackdaw_track_set_fader(JackDawTrack *t, gfloat fader);
gfloat jackdaw_track_get_fader(JackDawTrack *t);
void jackdaw_track_set_pan(JackDawTrack *t, gfloat pan);
gfloat jackdaw_track_get_pan(JackDawTrack *t);

/* Input routing */
void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx);
void jackdaw_track_set_midi_in(JackDawTrack *t, gint idx);

/* Stereo-input flag (main thread). Mono is the default. */
gboolean jackdaw_track_is_stereo(JackDawTrack *t);

/* Peak meter read (call from main thread) */
void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R);

G_END_DECLS

#endif /* TRACK_H_INCLUDED */
