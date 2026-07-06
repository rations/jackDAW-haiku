#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <jack/jack.h>

#include "rt_denormal.h" /* FTZ/DAZ for the RT thread */

#include "jackdaw-engine.h"
#include "settings.h"
#include "message.h"

/* -----------------------------------------------------------------------
 * Internal state
 *
 * Ported from the Linux JackDAW engine, reduced to the phase-1 surface:
 * transport flags + play_pos, count-in, metronome, stereo master outs.
 * The feeder/recorder threads, per-track capture/MIDI ports, plugin graph,
 * loop/punch logic and render taps of the original return in phase 2; their
 * state fields keep their slots so that code drops back in unchanged.
 * ----------------------------------------------------------------------- */

/* Transport control flags — written by main thread, read by RT callback.
 * Use g_atomic_int_* for all accesses. */
#define ENGINE_PLAYING (1 << 0)
#define ENGINE_RECORDING (1 << 1)

typedef struct {
    jack_client_t *client;
    JackDawProject *project; /* weak ref — project owns the engine */

    /* Audio output ports — out_1 (L master), out_2 (R master). */
    jack_port_t **audio_out;
    guint audio_out_count;

    /* Pre-allocated mix buffers (sized to buffer size at init) */
    float *master_L;
    float *master_R;
    jack_nframes_t buf_size; /* current buffer size */

    /* Weak refs to active tracks — slots populated by engine_add_track */
    JackDawTrack *slots[JACKDAW_MAX_TRACKS];

    volatile gint transport_flags; /* ENGINE_PLAYING | ENGINE_RECORDING */
    volatile off_t play_pos;       /* sample counter, incremented by process cb */

    /* Loop region (frames) — fields reserved for the phase-2 port of looping
     * and punch recording; nothing reads them yet. */
    volatile gint loop_enabled;
    volatile off_t loop_start;
    volatile off_t loop_end;

    /* Count-in pre-roll. While countin_active the transport is in a metronome-
     * only lead-in: the project is frozen (play_pos does not advance) and nothing
     * records. The metronome clicks from countin_pos; when it reaches countin_len
     * the pending transport (PLAYING, plus RECORDING if countin_pending_rec)
     * engages and normal play begins from the unchanged play_pos. Set up on the
     * main thread; the RT path drives countin_pos and performs the hand-off. */
    volatile gint countin_active;
    volatile gint countin_pending_rec;
    volatile off_t countin_pos;
    volatile off_t countin_len;

    jack_nframes_t sample_rate; /* cached at init */

    /* Pre-rendered metronome click (mono), built at init. */
    float *click_buf;
    int click_len;

    /* Dedicated mono metronome output port ("metronome"). The click is always
     * mirrored here so it can be routed to a performer's headphones independently
     * of the main mix; whether it ALSO reaches the main outs is set per-project
     * (metronome_route). Never part of the master sum or meters. */
    jack_port_t *metro_out;

    /* Post-master-fader peak meter (master VU). Racy read is acceptable. */
    volatile gfloat master_peak_L;
    volatile gfloat master_peak_R;

    gboolean active;
} JackDawEngine;

static JackDawEngine engine;

static volatile gint engine_xruns = 0;

/* Diagnostics (JACKDAW_DIAG): per-cycle callback timing + xrun reporter, so a
 * missed deadline can be attributed to this callback vs. external scheduling. */
static gboolean g_diag_on = FALSE;
static volatile gint64 g_diag_cb_last_us = 0;
static volatile gint64 g_diag_cb_max_us = 0;
static gint64 g_diag_period_us = 0;
static GThread *g_diag_thread = NULL;
static volatile gint g_diag_quit = 0;

static gpointer diag_thread_func(gpointer arg)
{
    (void)arg;
    int last_xruns = 0;
    while (!g_atomic_int_get(&g_diag_quit)) {
        g_usleep(1000000); /* 1 s */
        int x = g_atomic_int_get(&engine_xruns);
        int dx = x - last_xruns;
        last_xruns = x;
        gint64 cb_max = g_diag_cb_max_us;
        g_diag_cb_max_us = 0;
        g_message("[diag] xruns +%d (total %d)  cb_last=%ldus cb_max=%ldus period=%ldus", dx, x,
                  (long)g_diag_cb_last_us, (long)cb_max, (long)g_diag_period_us);
    }
    return NULL;
}

/* ---- UI event hook (see header) ---- */

static JackDawEngineEventHook engine_event_hook = NULL;
static void *engine_event_hook_user = NULL;

void jackdaw_engine_set_event_hook(JackDawEngineEventHook hook, void *user)
{
    engine_event_hook = hook;
    engine_event_hook_user = user;
}

/* Called from JACK notification threads — must not block, allocate, touch UI
 * or emit GObject signals. The hook only posts a message. */
static void engine_post_event(int event)
{
    JackDawEngineEventHook hook = engine_event_hook;
    if (hook)
        hook(event, engine_event_hook_user);
}

/* -----------------------------------------------------------------------
 * RT callbacks
 * ----------------------------------------------------------------------- */

static inline void engine_rt_set_denormal_mode(void)
{
    rt_set_denormal_mode();
}

/* JACK thread-init callback: runs once per RT thread JACK spawns, before any
 * process cycle. */
static void engine_thread_init_cb(void *arg)
{
    (void)arg;
    engine_rt_set_denormal_mode();
}

static int engine_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    guint i, oi;
    gint flags;
    float *port_buf;
    jack_nframes_t k;

    /* Diagnostics: time the whole cycle (JACKDAW_DIAG only). */
    gint64 diag_t0 = 0;
    if (g_diag_on)
        diag_t0 = g_get_monotonic_time();

    /* Flush denormals to zero on this RT thread (belt; thread-init cb is the
     * suspenders). */
    engine_rt_set_denormal_mode();

    /* Clear master mix buffers */
    memset(engine.master_L, 0, nframes * sizeof(float));
    memset(engine.master_R, 0, nframes * sizeof(float));

    flags = g_atomic_int_get(&engine.transport_flags);

    /* Count-in pre-roll: a metronome-only lead-in. While active the project is
     * frozen (play_pos does not advance) and nothing records; the metronome
     * clicks from countin_pos. When the pre-roll length elapses the pending
     * transport (PLAYING, optionally RECORDING) engages and normal play begins
     * this block from the unchanged play_pos. Period-granular. */
    gboolean preroll = FALSE;
    off_t preroll_base = 0;
    if (g_atomic_int_get(&engine.countin_active)) {
        if (engine.countin_pos >= engine.countin_len) {
            gint pend = g_atomic_int_get(&engine.countin_pending_rec);
            g_atomic_int_set(&engine.countin_active, 0);
            g_atomic_int_or(&engine.transport_flags,
                            ENGINE_PLAYING | (pend ? ENGINE_RECORDING : 0));
            flags = g_atomic_int_get(&engine.transport_flags);
        } else {
            preroll = TRUE;
            preroll_base = engine.countin_pos;
            engine.countin_pos += (off_t)nframes;
        }
    }

    if (flags & ENGINE_PLAYING)
        engine.play_pos += nframes;

    /* Phase 2 ports the per-track mixing passes (solo scan, worker fan-out,
     * live monitoring, capture) here. With no track audio yet the master sum
     * stays silent; the master fader and peak meter still run so the plumbing
     * downstream of them is real. */
    gfloat master_vol = engine.project ? engine.project->master_volume : 1.0f;
    float peak_L = 0.0f, peak_R = 0.0f;
    for (k = 0; k < nframes; k++) {
        engine.master_L[k] *= master_vol;
        engine.master_R[k] *= master_vol;
        float aL = fabsf(engine.master_L[k]);
        float aR = fabsf(engine.master_R[k]);
        if (aL > peak_L)
            peak_L = aL;
        if (aR > peak_R)
            peak_R = aR;
    }
    if (peak_L > engine.master_peak_L)
        engine.master_peak_L = peak_L;
    if (peak_R > engine.master_peak_R)
        engine.master_peak_R = peak_R;

    /* Copy the master mix to the output ports */
    for (i = 0; i < engine.audio_out_count; i++) {
        if (!engine.audio_out[i])
            continue;
        port_buf = jack_port_get_buffer(engine.audio_out[i], nframes);
        memcpy(port_buf, (i == 0) ? engine.master_L : engine.master_R, nframes * sizeof(float));
    }

    /* Metronome click — monitored by mixing straight onto the audio outputs,
     * AFTER the master fader and the master peak meter. The click is therefore
     * audible but completely separate from the project signal: it cannot raise
     * the master meters and is independent of the master volume/mute.
     *
     * The dedicated "metronome" port always carries the click (a standalone feed
     * the user can route to a performer's headphones). metronome_route decides
     * whether the click ALSO bleeds into the main outs (MAIN) or stays on the
     * dedicated port only (CLICK_PORT = "headphones only"). The metro port is
     * cleared every cycle so it never plays stale buffer contents. */
    {
        float *metro_buf =
            engine.metro_out ? (float *)jack_port_get_buffer(engine.metro_out, nframes) : NULL;
        if (metro_buf)
            memset(metro_buf, 0, nframes * sizeof(float));

        /* The click sounds either for the normal metronome (playing + enabled) or
         * for a count-in pre-roll (always, regardless of the metronome toggle —
         * the count-in IS the click). The two cases differ only in the position
         * the beat grid is measured from. */
        gboolean play_click =
            (flags & ENGINE_PLAYING) && engine.project && engine.project->metronome_enabled;
        if ((play_click || preroll) && engine.project && engine.click_buf && engine.click_len > 0 &&
            engine.project->bpm > 0.0) {
            gboolean to_main = engine.project->metronome_route == METRONOME_ROUTE_MAIN;
            double fpb = (double)engine.sample_rate * 60.0 / engine.project->bpm;
            guint bpb = engine.project->beats_per_bar ? engine.project->beats_per_bar : 4;
            float click_gain = engine.project->metronome_gain;
            if (fpb > 1.0) {
                off_t base = preroll ? preroll_base : engine.play_pos - (off_t)nframes;
                float *out_buf[2] = {NULL, NULL};
                if (to_main) {
                    for (oi = 0; oi < engine.audio_out_count && oi < 2; oi++)
                        if (engine.audio_out[oi])
                            out_buf[oi] = jack_port_get_buffer(engine.audio_out[oi], nframes);
                }
                for (k = 0; k < nframes; k++) {
                    off_t a = base + (off_t)k;
                    if (a < 0)
                        continue;
                    off_t beat = (off_t)((double)a / fpb);
                    off_t boundary = (off_t)((double)beat * fpb + 0.5);
                    off_t off = a - boundary;
                    if (off >= 0 && off < engine.click_len) {
                        float s = engine.click_buf[off] * click_gain;
                        if ((beat % (off_t)bpb) != 0)
                            s *= 0.45f; /* accent downbeat */
                        if (metro_buf)
                            metro_buf[k] += s;
                        if (out_buf[0])
                            out_buf[0][k] += s;
                        if (out_buf[1])
                            out_buf[1][k] += s;
                    }
                }
            }
        }
    }

    if (diag_t0) {
        gint64 d = g_get_monotonic_time() - diag_t0;
        g_diag_cb_last_us = d;
        if (d > g_diag_cb_max_us)
            g_diag_cb_max_us = d;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Buffer size callback — called by JACK when buffer size changes.
 * Must NOT block; reallocates mix buffers.
 * ----------------------------------------------------------------------- */
static int engine_buffer_size_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    /* Reallocate mix scratch buffers */
    g_free(engine.master_L);
    g_free(engine.master_R);
    engine.master_L = g_malloc0(nframes * sizeof(float));
    engine.master_R = g_malloc0(nframes * sizeof(float));
    engine.buf_size = nframes;

    return 0;
}

static void engine_shutdown_cb(void *arg)
{
    (void)arg;
    engine.active = FALSE;
    engine_post_event(JACKDAW_ENGINE_EVENT_SHUTDOWN);
}

static int engine_xrun_cb(void *arg)
{
    (void)arg;
    g_atomic_int_inc(&engine_xruns);
    return 0;
}

/* ---- Port topology callbacks (JACK notification thread → UI hook) ---- */

static void engine_port_reg_cb(jack_port_id_t port_id, int registered, void *arg)
{
    (void)port_id;
    (void)registered;
    (void)arg;
    engine_post_event(JACKDAW_ENGINE_EVENT_PORTS_CHANGED);
}

static void engine_port_connect_cb(jack_port_id_t a, jack_port_id_t b, int connect, void *arg)
{
    (void)a;
    (void)b;
    (void)connect;
    (void)arg;
    engine_post_event(JACKDAW_ENGINE_EVENT_CONNECTIONS_CHANGED);
}

/* -----------------------------------------------------------------------
 * Init / quit
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_init(JackDawProject *project)
{
    jack_status_t status;
    jack_nframes_t sr, bs;
    char name[64];
    guint i;

    if (engine.active)
        return FALSE; /* already running */

    memset(&engine, 0, sizeof(engine));
    engine.project = project;

    /* Phase 1: fixed stereo master. The Linux original auto-detects port
     * counts from the physical JACK ports / settings; that returns in phase 2
     * together with the capture and MIDI ports. */
    engine.audio_out_count = 2;

    engine.client = jack_client_open("jackdaw", JackNullOption, &status);
    if (!engine.client) {
        jackdaw_error("Could not connect to JACK server.\n"
                      "Is jackd running?");
        return TRUE;
    }

    /* Query JACK's actual sample rate and buffer size */
    sr = jack_get_sample_rate(engine.client);
    bs = jack_get_buffer_size(engine.client);
    engine.buf_size = bs;
    engine.sample_rate = sr;

    /* Pre-render the metronome click: a 1 kHz tone with a fast decay. */
    engine.click_len = (int)((double)sr * 0.025);
    engine.click_buf = g_malloc0((size_t)engine.click_len * sizeof(float));
    for (int n = 0; n < engine.click_len; n++) {
        double tt = (double)n / (double)sr;
        double env = exp(-tt * 120.0);
        engine.click_buf[n] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * tt) * env);
    }

    /* Allocate mix scratch buffers */
    engine.master_L = g_malloc0(bs * sizeof(float));
    engine.master_R = g_malloc0(bs * sizeof(float));

    /* Register callbacks */
    jack_set_thread_init_callback(engine.client, engine_thread_init_cb, NULL);
    jack_set_process_callback(engine.client, engine_process, NULL);
    jack_set_buffer_size_callback(engine.client, engine_buffer_size_cb, NULL);
    jack_set_xrun_callback(engine.client, engine_xrun_cb, NULL);
    jack_on_shutdown(engine.client, engine_shutdown_cb, NULL);
    jack_set_port_registration_callback(engine.client, engine_port_reg_cb, NULL);
    jack_set_port_connect_callback(engine.client, engine_port_connect_cb, NULL);

    /* Register audio output ports: out_1 .. out_N */
    engine.audio_out = g_new0(jack_port_t *, engine.audio_out_count);
    for (i = 0; i < engine.audio_out_count; i++) {
        g_snprintf(name, sizeof(name), "out_%u", i + 1);
        engine.audio_out[i] =
            jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!engine.audio_out[i])
            goto fail;
    }

    /* Dedicated metronome output (mono). Routed by the user (e.g. to a
     * performer's headphone bus) via the patchbay; not auto-connected. */
    engine.metro_out = jack_port_register(engine.client, "metronome", JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsOutput, 0);
    if (!engine.metro_out)
        goto fail;

    /* Activate — after this the process callback can be called at any time */
    if (jack_activate(engine.client) != 0) {
        jackdaw_error("jackdaw: jack_activate() failed");
        goto fail;
    }

    /* Auto-connect out_N → physical audio playback ports by matching index, so
     * out_1→playback_1, out_2→playback_2, ... Don't rely on JACK's own
     * auto-connect (it's off unless the client requests it and many setups
     * disable it), so wire it here. EEXIST is fine. */
    {
        const char **phys_audio_in = jack_get_ports(engine.client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsInput | JackPortIsPhysical);
        if (phys_audio_in) {
            guint pi;
            for (pi = 0; pi < engine.audio_out_count && phys_audio_in[pi]; pi++) {
                const char *src = jack_port_name(engine.audio_out[pi]);
                int r = jack_connect(engine.client, src, phys_audio_in[pi]);
                (void)r; /* EEXIST is fine */
            }
            jack_free(phys_audio_in);
        }
    }

    engine.active = TRUE;

    /* Diagnostics: only when JACKDAW_DIAG is set. */
    g_diag_on = (g_getenv("JACKDAW_DIAG") != NULL);
    if (g_diag_on) {
        g_diag_period_us = sr ? (gint64)bs * 1000000 / sr : 0;
        g_atomic_int_set(&g_diag_quit, 0);
        g_diag_thread = g_thread_new("jackdaw-diag", diag_thread_func, NULL);
        g_message("[diag] enabled: buf=%u period=%ldus — watching xruns / callback time", bs,
                  (long)g_diag_period_us);
    }
    return FALSE; /* success */

fail:
    jack_client_close(engine.client);
    engine.client = NULL;
    g_free(engine.master_L);
    engine.master_L = NULL;
    g_free(engine.master_R);
    engine.master_R = NULL;
    g_free(engine.click_buf);
    engine.click_buf = NULL;
    engine.click_len = 0;
    g_free(engine.audio_out);
    engine.audio_out = NULL;
    return TRUE;
}

void jackdaw_engine_quit(void)
{
    if (!engine.active || !engine.client)
        return;

    if (g_diag_thread) {
        g_atomic_int_set(&g_diag_quit, 1);
        g_thread_join(g_diag_thread);
        g_diag_thread = NULL;
    }

    jack_deactivate(engine.client);
    jack_client_close(engine.client);
    engine.client = NULL;
    engine.active = FALSE;

    g_free(engine.master_L);
    engine.master_L = NULL;
    g_free(engine.master_R);
    engine.master_R = NULL;
    g_free(engine.click_buf);
    engine.click_buf = NULL;
    engine.click_len = 0;
    g_free(engine.audio_out);
    engine.audio_out = NULL;
    engine.metro_out = NULL; /* unregistered by jack_client_close above */
}

/* -----------------------------------------------------------------------
 * State queries
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_is_running(void)
{
    return engine.active;
}

gboolean jackdaw_engine_is_recording(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_RECORDING) != 0;
}

gboolean jackdaw_engine_is_playing(void)
{
    return (g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING) != 0;
}

/* -----------------------------------------------------------------------
 * Track management
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_add_track(JackDawTrack *track)
{
    guint i;

    g_return_val_if_fail(JACKDAW_IS_TRACK(track), TRUE);

    /* Find a free slot */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        if (!engine.slots[i])
            break;
    }
    if (i == JACKDAW_MAX_TRACKS) {
        jackdaw_error("jackdaw: maximum track count reached");
        return TRUE;
    }

    /* Phase 2 allocates the playback/record ringbuffers and assigns capture
     * ports here; with no capture ports yet every input index stays -1. */
    track->slot = i;
    engine.slots[i] = track; /* RT callback can see this now */
    track->audio_in_idx = -1;
    track->midi_in_idx = -1;

    return FALSE;
}

void jackdaw_engine_remove_track(JackDawTrack *track)
{
    guint i;
    g_return_if_fail(JACKDAW_IS_TRACK(track));
    i = track->slot;
    if (i >= JACKDAW_MAX_TRACKS || engine.slots[i] != track)
        return;

    engine.slots[i] = NULL; /* RT callback stops using this slot */

    track->slot = G_MAXUINT;
}

/* -----------------------------------------------------------------------
 * Transport
 * ----------------------------------------------------------------------- */

void jackdaw_engine_start_playback(void)
{
    g_atomic_int_or(&engine.transport_flags, ENGINE_PLAYING);
}

void jackdaw_engine_stop_playback(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_PLAYING);

    /* Cancel a count-in pre-roll that never reached its hand-off. */
    if (g_atomic_int_get(&engine.countin_active)) {
        g_atomic_int_set(&engine.countin_active, 0);
        g_atomic_int_set(&engine.countin_pending_rec, 0);
    }
}

void jackdaw_engine_start_recording(void)
{
    /* Phase 2 pre-opens the per-track capture slots here (recorder_arm_all).
     * Start rolling — flags only for now. */
    g_atomic_int_or(&engine.transport_flags, ENGINE_RECORDING | ENGINE_PLAYING);
}

void jackdaw_engine_stop_recording(void)
{
    g_atomic_int_and(&engine.transport_flags, ~ENGINE_RECORDING);

    /* A record count-in that hasn't engaged yet: clear it so the pre-roll won't
     * hand off into recording. */
    g_atomic_int_set(&engine.countin_active, 0);
    g_atomic_int_set(&engine.countin_pending_rec, 0);
}

/* Begin a count-in pre-roll, then start playback (record=FALSE) or recording
 * (record=TRUE) when it elapses. `beats` is the number of metronome clicks to
 * sound first. Returns FALSE (caller should start immediately) when no pre-roll
 * is possible: engine not running, beats==0, or a degenerate tempo. The project
 * playhead stays put during the pre-roll, so when recording the take begins
 * exactly at the cursor. */
gboolean jackdaw_engine_begin_countin(guint beats, gboolean record)
{
    if (!engine.active || beats == 0)
        return FALSE;
    if (g_atomic_int_get(&engine.countin_active))
        return TRUE; /* already counting */

    double bpm = (engine.project && engine.project->bpm > 0.0) ? engine.project->bpm : 120.0;
    double fpb = (double)engine.sample_rate * 60.0 / bpm;
    off_t len = (off_t)(fpb * (double)beats + 0.5);
    if (len <= 0)
        return FALSE;

    engine.countin_pos = 0;
    engine.countin_len = len;
    g_atomic_int_set(&engine.countin_pending_rec, record ? 1 : 0);
    g_atomic_int_set(&engine.countin_active, 1); /* publish last */
    return TRUE;
}

gboolean jackdaw_engine_is_counting_in(void)
{
    return g_atomic_int_get(&engine.countin_active) != 0;
}

void jackdaw_engine_locate(off_t sample)
{
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    engine.play_pos = sample;
    /* Phase 2 re-seeks every feeder slot to the new position here. */
}

/* -----------------------------------------------------------------------
 * Queries
 * ----------------------------------------------------------------------- */

jack_nframes_t jackdaw_engine_get_sample_rate(void)
{
    if (!engine.client)
        return 48000;
    return jack_get_sample_rate(engine.client);
}

jack_nframes_t jackdaw_engine_get_buffer_size(void)
{
    if (!engine.client)
        return 1024;
    return jack_get_buffer_size(engine.client);
}

off_t jackdaw_engine_get_play_pos(void)
{
    return engine.play_pos;
}

void jackdaw_engine_get_master_peaks(gfloat *out_L, gfloat *out_R)
{
    if (out_L) {
        *out_L = engine.master_peak_L;
        engine.master_peak_L = 0.0f;
    }
    if (out_R) {
        *out_R = engine.master_peak_R;
        engine.master_peak_R = 0.0f;
    }
}

guint jackdaw_engine_get_xrun_count(void)
{
    return (guint)g_atomic_int_get(&engine_xruns);
}
