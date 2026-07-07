#include <string.h>
#include "track.h"
#include "message.h"

G_DEFINE_TYPE(JackDawTrack, jackdaw_track, G_TYPE_OBJECT)

enum { SIGNAL_STATE_CHANGED, SIGNAL_ROUTING_CHANGED, LAST_SIGNAL };

static guint track_signals[LAST_SIGNAL];

/* ---- GObject boilerplate ---- */

static void jackdaw_track_finalize(GObject *obj)
{
    JackDawTrack *t = JACKDAW_TRACK(obj);

    g_free(t->name);
    g_free(t->audio_src_port);
    g_free(t->audio_src_port_r);
    g_free(t->midi_src_port);

    G_OBJECT_CLASS(jackdaw_track_parent_class)->finalize(obj);
}

static void jackdaw_track_class_init(JackDawTrackClass *klass)
{
    GObjectClass *gc = G_OBJECT_CLASS(klass);
    gc->finalize = jackdaw_track_finalize;

    track_signals[SIGNAL_STATE_CHANGED] = g_signal_new(
        "state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawTrackClass, state_changed), NULL, NULL, NULL, G_TYPE_NONE, 0);

    track_signals[SIGNAL_ROUTING_CHANGED] = g_signal_new(
        "routing-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET(JackDawTrackClass, routing_changed), NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void jackdaw_track_init(JackDawTrack *t)
{
    t->name = NULL;
    t->slot = G_MAXUINT;
    t->kind = JACKDAW_TRACK_AUDIO;
    t->audio_in_idx = -1;
    t->midi_in_idx = -1;
    t->audio_src_port = NULL;
    t->audio_src_port_r = NULL;
    t->midi_src_port = NULL;
    t->stereo_input = FALSE;
    t->state_flags = 0;
    t->volume = 1.0f;
    t->trim = 1.0f;
    t->fader = 1.0f;
    t->pan = 0.0f;
    t->peak_L = 0.0f;
    t->peak_R = 0.0f;
}

/* ---- Constructor ---- */

JackDawTrack *jackdaw_track_new(const gchar *name)
{
    JackDawTrack *t = g_object_new(JACKDAW_TYPE_TRACK, NULL);

    t->name = g_strdup(name ? name : "Track");

    return t;
}

/* ---- Accessors ---- */

const gchar *jackdaw_track_get_name(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), NULL);
    return t->name;
}

void jackdaw_track_set_name(JackDawTrack *t, const gchar *name)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    g_free(t->name);
    t->name = g_strdup(name ? name : "Track");
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

/* ---- Track kind ---- */

JackDawTrackKind jackdaw_track_get_kind(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), JACKDAW_TRACK_AUDIO);
    return t->kind;
}

void jackdaw_track_set_kind(JackDawTrack *t, JackDawTrackKind kind)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->kind = kind;
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean jackdaw_track_is_instrument(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), FALSE);
    return t->kind == JACKDAW_TRACK_INSTRUMENT;
}

/* ---- State flags ---- */

void jackdaw_track_set_armed(JackDawTrack *t, gboolean armed)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (armed)
        g_atomic_int_or(&t->state_flags, TRACK_ARMED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_ARMED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_muted(JackDawTrack *t, gboolean muted)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (muted)
        g_atomic_int_or(&t->state_flags, TRACK_MUTED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_MUTED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

void jackdaw_track_set_soloed(JackDawTrack *t, gboolean soloed)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    if (soloed)
        g_atomic_int_or(&t->state_flags, TRACK_SOLOED);
    else
        g_atomic_int_and(&t->state_flags, ~TRACK_SOLOED);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gboolean jackdaw_track_is_armed(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_ARMED) != 0;
}

gboolean jackdaw_track_is_muted(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_MUTED) != 0;
}

gboolean jackdaw_track_is_soloed(JackDawTrack *t)
{
    return (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED) != 0;
}

/* ---- Volume / pan ---- */

/* Fold the two stages into the effective gain the RT callback reads. */
static void track_recompute_volume(JackDawTrack *t)
{
    gfloat v = t->trim * t->fader;
    t->volume = CLAMP(v, 0.0f, 18.0f); /* 18.0 ≈ linear gain for +25 dB */
}

void jackdaw_track_set_volume(JackDawTrack *t, gfloat vol)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    /* Legacy entry point: treat the value as the fader stage, trim stays unity. */
    t->fader = CLAMP(vol, 0.0f, 18.0f);
    track_recompute_volume(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gfloat jackdaw_track_get_volume(JackDawTrack *t)
{
    return t->volume;
}

void jackdaw_track_set_trim(JackDawTrack *t, gfloat trim)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->trim = CLAMP(trim, 0.0f, 18.0f);
    track_recompute_volume(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gfloat jackdaw_track_get_trim(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 1.0f);
    return t->trim;
}

void jackdaw_track_set_fader(JackDawTrack *t, gfloat fader)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->fader = CLAMP(fader, 0.0f, 18.0f);
    track_recompute_volume(t);
    g_signal_emit(t, track_signals[SIGNAL_STATE_CHANGED], 0);
}

gfloat jackdaw_track_get_fader(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), 1.0f);
    return t->fader;
}

void jackdaw_track_set_pan(JackDawTrack *t, gfloat pan)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->pan = CLAMP(pan, -1.0f, 1.0f);
}

gfloat jackdaw_track_get_pan(JackDawTrack *t)
{
    return t->pan;
}

/* ---- Input routing ---- */

void jackdaw_track_set_audio_in(JackDawTrack *t, gint idx)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->audio_in_idx = idx;
    g_signal_emit(t, track_signals[SIGNAL_ROUTING_CHANGED], 0);
}

void jackdaw_track_set_midi_in(JackDawTrack *t, gint idx)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    t->midi_in_idx = idx;
    g_signal_emit(t, track_signals[SIGNAL_ROUTING_CHANGED], 0);
}

gboolean jackdaw_track_is_stereo(JackDawTrack *t)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), FALSE);
    return t->stereo_input;
}

/* ---- Peak metering ---- */

void jackdaw_track_get_peaks(JackDawTrack *t, gfloat *out_L, gfloat *out_R)
{
    g_return_if_fail(JACKDAW_IS_TRACK(t));
    /* Non-destructive read: the RT callback applies decay-hold, so multiple
     * meters (track strip + mixer) can poll this concurrently. */
    if (out_L)
        *out_L = t->peak_L;
    if (out_R)
        *out_R = t->peak_R;
}
