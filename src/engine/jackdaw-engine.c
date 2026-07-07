#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

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

    /* Audio capture ports — a pool of mono inputs. audio_in[i] is track i's
     * LEFT/mono capture; audio_in_r[i] its RIGHT capture (stereo tracks only).
     * A track reads the pair at its audio_in_idx. */
    jack_port_t **audio_in;
    jack_port_t **audio_in_r;
    guint audio_in_count;

    /* MIDI capture ports, one per instrument-track slot, registered lazily when
     * an instrument track is added (midi_in[slot]). A NULL slot has no MIDI
     * input. Sized JACKDAW_MAX_TRACKS. */
    jack_port_t **midi_in;

    /* Pre-allocated mix buffers (sized to buffer size at init) */
    float *master_L;
    float *master_R;
    /* Per-track scratch, reused sequentially (single-threaded mix pass). */
    float *track_L;
    float *track_R;
    jack_nframes_t buf_size; /* current buffer size */

    /* Weak refs to active tracks — slots populated by engine_add_track */
    JackDawTrack *slots[JACKDAW_MAX_TRACKS];

    volatile gint transport_flags; /* ENGINE_PLAYING | ENGINE_RECORDING */
    volatile off_t play_pos;       /* sample counter, incremented by process cb */

    /* Loop region (frames). Looping is active only while loop_enabled is set and
     * loop_end > loop_start; the playhead wraps loop_end -> loop_start once it
     * has entered the region. Written on the window thread, read by RT. */
    volatile gint loop_enabled;
    volatile off_t loop_start;
    volatile off_t loop_end;

    /* Record mode (RECORD_MODE_NORMAL / RECORD_MODE_PUNCH). Punch capture over
     * the loop-tab region is wired in the recording phase; stored here now so
     * the RT capture path can read it. Window thread writes, RT reads. */
    volatile gint record_mode;

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

    /* Loop wrap (master clock — drives the metronome and the UI playhead, and
     * later MIDI scheduling and the feeder). Only engages when this block
     * started inside the region: block-start in [loop_start, loop_end). A
     * playhead placed after the region therefore plays straight through without
     * looping. The remainder past loop_end is carried over so the loop period
     * equals the region length. Checked at block granularity, so the loop point
     * quantizes to the JACK period (acceptable for now). */
    if ((flags & ENGINE_PLAYING) && g_atomic_int_get(&engine.loop_enabled)) {
        off_t l_start = engine.loop_start;
        off_t l_end = engine.loop_end;
        off_t bstart = engine.play_pos - (off_t)nframes;
        if (l_end > l_start && bstart >= l_start && bstart < l_end && engine.play_pos >= l_end)
            engine.play_pos = l_start + (engine.play_pos - l_end);
    }

    /* Per-track mix pass (single-threaded; the work-stealing pool is a later
     * optimisation — see the plan's flagged assumptions). For each active track:
     * pull its live input (armed audio tracks with a capture port), apply the
     * constant-power pan + effective fader, meter post-fader, and sum into the
     * master. Clip playback (regions) and FX chains fold in at their phases.
     *
     * Solo scan first: if any track is soloed, non-soloed tracks are muted. */
    gboolean any_soloed = FALSE;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (t && (g_atomic_int_get(&t->state_flags) & TRACK_SOLOED)) {
            any_soloed = TRUE;
            break;
        }
    }

    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t)
            continue;

        float *bL = engine.track_L;
        float *bR = engine.track_R;
        memset(bL, 0, nframes * sizeof(float));
        memset(bR, 0, nframes * sizeof(float));

        gint tflags = g_atomic_int_get(&t->state_flags);
        gboolean muted = (tflags & TRACK_MUTED) || (any_soloed && !(tflags & TRACK_SOLOED));

        /* Live input monitor: armed audio track with an assigned capture port.
         * jack_port_get_buffer must be called from the RT (process) thread —
         * this is it. A mono track duplicates its one input to both channels. */
        if (!jackdaw_track_is_instrument(t) && (tflags & TRACK_ARMED) && t->audio_in_idx >= 0 &&
            (guint)t->audio_in_idx < engine.audio_in_count &&
            engine.audio_in[(guint)t->audio_in_idx]) {
            float *live_L =
                (float *)jack_port_get_buffer(engine.audio_in[(guint)t->audio_in_idx], nframes);
            float *live_R = NULL;
            if (t->audio_src_port_r && engine.audio_in_r[(guint)t->audio_in_idx])
                live_R = (float *)jack_port_get_buffer(engine.audio_in_r[(guint)t->audio_in_idx],
                                                       nframes);
            if (live_L && !live_R)
                live_R = live_L;
            if (live_L) {
                for (k = 0; k < nframes; k++) {
                    bL[k] += live_L[k];
                    bR[k] += live_R[k];
                }
            }
        }

        /* Constant-power pan + effective fader; meter post-fader with a decay
         * hold so the strip/mixer VU falls back smoothly between peaks. */
        gfloat vol = muted ? 0.0f : t->volume;
        float angle = (t->pan + 1.0f) * (float)M_PI_4;
        float gain_L = vol * cosf(angle);
        float gain_R = vol * sinf(angle);
        float peak_L = 0.0f, peak_R = 0.0f;
        for (k = 0; k < nframes; k++) {
            float sL = bL[k] * gain_L;
            float sR = bR[k] * gain_R;
            engine.master_L[k] += sL;
            engine.master_R[k] += sR;
            float aL = fabsf(sL), aR = fabsf(sR);
            if (aL > peak_L)
                peak_L = aL;
            if (aR > peak_R)
                peak_R = aR;
        }
        t->peak_L = (peak_L > t->peak_L) ? peak_L : t->peak_L * 0.92f;
        t->peak_R = (peak_R > t->peak_R) ? peak_R : t->peak_R * 0.92f;
    }

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
    g_free(engine.track_L);
    g_free(engine.track_R);
    engine.master_L = g_malloc0(nframes * sizeof(float));
    engine.master_R = g_malloc0(nframes * sizeof(float));
    engine.track_L = g_malloc0(nframes * sizeof(float));
    engine.track_R = g_malloc0(nframes * sizeof(float));
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

/* Count physical ports of the given type/direction (>=1). Hardware capture
 * ports are JackPortIsOutput from a client's perspective (you connect from
 * them); playback ports are JackPortIsInput. */
static guint count_physical_ports(jack_client_t *c, const char *type, unsigned long flags)
{
    const char **ports = jack_get_ports(c, NULL, type, flags | JackPortIsPhysical);
    guint n = 0;
    if (ports) {
        for (; ports[n]; n++)
            ;
        jack_free(ports);
    }
    return n ? n : 1;
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

    /* JackNoStartServer: connect only to the server the user already started
     * (e.g. from jack-graph's settings), never spawn or reconfigure one of our
     * own. If no server is running this fails cleanly and the user is told to
     * start jackd first — JackDAW is a plain client, not a server manager. */
    engine.client = jack_client_open("jackdaw", JackNoStartServer, &status);
    if (!engine.client) {
        jackdaw_error("Could not connect to the JACK server.\n"
                      "Start JACK first (e.g. from jack-graph), then launch JackDAW.");
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
    engine.track_L = g_malloc0(bs * sizeof(float));
    engine.track_R = g_malloc0(bs * sizeof(float));

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

    /* Capture-port pool: one mono input ("in_N") per possible track, sized to
     * the number of physical capture channels the server exposes (clamped) so
     * every hardware input can be routed to a track. Right ports are registered
     * lazily by set_track_stereo. A track at slot i reads in_(i+1). */
    engine.audio_in_count =
        CLAMP(count_physical_ports(engine.client, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput), 1,
              JACKDAW_MAX_TRACKS);
    engine.audio_in = g_new0(jack_port_t *, engine.audio_in_count);
    engine.audio_in_r = g_new0(jack_port_t *, engine.audio_in_count);
    for (i = 0; i < engine.audio_in_count; i++) {
        g_snprintf(name, sizeof(name), "in_%u", i + 1);
        engine.audio_in[i] =
            jack_port_register(engine.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!engine.audio_in[i])
            goto fail;
    }

    /* MIDI capture ports are registered on demand (per instrument-track slot). */
    engine.midi_in = g_new0(jack_port_t *, JACKDAW_MAX_TRACKS);

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
            /* Also monitor the dedicated metronome click through the same
             * physical outputs (mono -> both), so it is audible by default
             * while staying out of the DAW's main mix (never summed into the
             * master, meters or recordings). The user can rewire it to a
             * separate headphone bus in the patchbay. EEXIST is fine. */
            if (engine.metro_out) {
                const char *msrc = jack_port_name(engine.metro_out);
                for (guint mi = 0; mi < 2 && phys_audio_in[mi]; mi++)
                    (void)jack_connect(engine.client, msrc, phys_audio_in[mi]);
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
    g_free(engine.track_L);
    engine.track_L = NULL;
    g_free(engine.track_R);
    engine.track_R = NULL;
    g_free(engine.click_buf);
    engine.click_buf = NULL;
    engine.click_len = 0;
    g_free(engine.audio_out);
    engine.audio_out = NULL;
    g_free(engine.audio_in);
    engine.audio_in = NULL;
    g_free(engine.audio_in_r);
    engine.audio_in_r = NULL;
    engine.audio_in_count = 0;
    g_free(engine.midi_in);
    engine.midi_in = NULL;
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
    g_free(engine.track_L);
    engine.track_L = NULL;
    g_free(engine.track_R);
    engine.track_R = NULL;
    g_free(engine.click_buf);
    engine.click_buf = NULL;
    engine.click_len = 0;
    g_free(engine.audio_out);
    engine.audio_out = NULL;
    g_free(engine.audio_in);
    engine.audio_in = NULL;
    g_free(engine.audio_in_r);
    engine.audio_in_r = NULL;
    engine.audio_in_count = 0;
    g_free(engine.midi_in);
    engine.midi_in = NULL;
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

    /* An audio track takes capture port in_(slot+1) as its input (if the pool
     * covers this slot). Instrument tracks take no audio input. The
     * playback/record ringbuffers are added with clips/recording (later phases).
     * Publish the slot last so the RT callback only sees a fully wired track. */
    if (jackdaw_track_is_instrument(track)) {
        track->audio_in_idx = -1;
        /* Register this instrument track's own MIDI capture port (midi_in_N). */
        track->midi_in_idx = (gint)i;
        if (engine.client && engine.midi_in && !engine.midi_in[i]) {
            char mname[64];
            g_snprintf(mname, sizeof(mname), "midi_in_%u", i + 1);
            engine.midi_in[i] = jack_port_register(engine.client, mname, JACK_DEFAULT_MIDI_TYPE,
                                                   JackPortIsInput, 0);
        }
    } else {
        track->audio_in_idx = (i < engine.audio_in_count) ? (gint)i : -1;
        track->midi_in_idx = -1;
    }
    track->slot = i;
    engine.slots[i] = track; /* RT callback can see this now */

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

    /* Tear down any jackdaw-made input connections and the lazily-registered
     * right capture port before releasing the slot. */
    if (engine.active && engine.client && track->audio_in_idx >= 0 &&
        (guint)track->audio_in_idx < engine.audio_in_count) {
        guint ai = (guint)track->audio_in_idx;
        if (track->audio_src_port && engine.audio_in[ai])
            jack_disconnect(engine.client, track->audio_src_port,
                            jack_port_name(engine.audio_in[ai]));
        if (track->audio_src_port_r && engine.audio_in_r[ai])
            jack_disconnect(engine.client, track->audio_src_port_r,
                            jack_port_name(engine.audio_in_r[ai]));
        if (engine.audio_in_r[ai]) {
            jack_port_unregister(engine.client, engine.audio_in_r[ai]);
            engine.audio_in_r[ai] = NULL;
        }
    }
    /* Tear down the instrument track's MIDI capture port + connection. */
    if (engine.active && engine.client && engine.midi_in && i < JACKDAW_MAX_TRACKS &&
        engine.midi_in[i]) {
        if (track->midi_src_port)
            jack_disconnect(engine.client, track->midi_src_port, jack_port_name(engine.midi_in[i]));
        jack_port_unregister(engine.client, engine.midi_in[i]);
        engine.midi_in[i] = NULL;
    }
    g_clear_pointer(&track->audio_src_port, g_free);
    g_clear_pointer(&track->audio_src_port_r, g_free);
    g_clear_pointer(&track->midi_src_port, g_free);
    track->stereo_input = FALSE;

    track->audio_in_idx = -1;
    track->midi_in_idx = -1;
    track->slot = G_MAXUINT;
}

/* -----------------------------------------------------------------------
 * Input routing
 * ----------------------------------------------------------------------- */

gboolean jackdaw_engine_set_audio_source_l(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client)
        return TRUE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count || !engine.audio_in[(guint)ai])
        return TRUE;

    const char *dst = jack_port_name(engine.audio_in[(guint)ai]);

    /* Disconnect the current source, if any, first. */
    if (t->audio_src_port) {
        jack_disconnect(engine.client, t->audio_src_port, dst);
        g_clear_pointer(&t->audio_src_port, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        t->audio_src_port = g_strdup(port_name);
    }
    g_signal_emit_by_name(t, "routing-changed");
    return FALSE;
}

gboolean jackdaw_engine_set_audio_source_r(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client)
        return TRUE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count || !engine.audio_in_r[(guint)ai])
        return TRUE;

    const char *dst = jack_port_name(engine.audio_in_r[(guint)ai]);

    if (t->audio_src_port_r) {
        jack_disconnect(engine.client, t->audio_src_port_r, dst);
        g_clear_pointer(&t->audio_src_port_r, g_free);
    }

    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        t->audio_src_port_r = g_strdup(port_name);
    }
    g_signal_emit_by_name(t, "routing-changed");
    return FALSE;
}

gboolean jackdaw_engine_set_track_stereo(JackDawTrack *t, gboolean stereo)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    t->stereo_input = stereo;

    if (!engine.active || !engine.client)
        return FALSE;

    gint ai = t->audio_in_idx;
    if (ai < 0 || (guint)ai >= engine.audio_in_count)
        return FALSE;

    if (stereo) {
        if (!engine.audio_in_r[(guint)ai]) {
            char name[64];
            g_snprintf(name, sizeof(name), "in_%uR", (guint)ai + 1);
            engine.audio_in_r[(guint)ai] = jack_port_register(
                engine.client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            if (!engine.audio_in_r[(guint)ai])
                return TRUE;
        }
    } else {
        /* Clear the right source (so RT stops reading it), then drop the port. */
        if (t->audio_src_port_r) {
            if (engine.audio_in_r[(guint)ai])
                jack_disconnect(engine.client, t->audio_src_port_r,
                                jack_port_name(engine.audio_in_r[(guint)ai]));
            g_clear_pointer(&t->audio_src_port_r, g_free);
        }
        if (engine.audio_in_r[(guint)ai]) {
            jack_port_unregister(engine.client, engine.audio_in_r[(guint)ai]);
            engine.audio_in_r[(guint)ai] = NULL;
        }
    }
    return FALSE;
}

gboolean jackdaw_engine_set_midi_source(JackDawTrack *t, const gchar *port_name)
{
    g_return_val_if_fail(JACKDAW_IS_TRACK(t), TRUE);
    if (!engine.active || !engine.client)
        return TRUE;

    gint mi = t->midi_in_idx;
    if (mi < 0 || mi >= JACKDAW_MAX_TRACKS || !engine.midi_in || !engine.midi_in[mi])
        return TRUE;

    const char *dst = jack_port_name(engine.midi_in[mi]);
    if (t->midi_src_port) {
        jack_disconnect(engine.client, t->midi_src_port, dst);
        g_clear_pointer(&t->midi_src_port, g_free);
    }
    if (port_name && *port_name) {
        int r = jack_connect(engine.client, port_name, dst);
        if (r != 0 && r != EEXIST)
            return TRUE;
        t->midi_src_port = g_strdup(port_name);
    }
    g_signal_emit_by_name(t, "routing-changed");
    return FALSE;
}

const char **jackdaw_engine_list_capture_ports(void)
{
    if (!engine.active || !engine.client)
        return NULL;
    return jack_get_ports(engine.client, NULL, JACK_DEFAULT_AUDIO_TYPE,
                          JackPortIsOutput | JackPortIsPhysical);
}

const char **jackdaw_engine_list_midi_ports(void)
{
    if (!engine.active || !engine.client)
        return NULL;
    return jack_get_ports(engine.client, NULL, JACK_DEFAULT_MIDI_TYPE,
                          JackPortIsOutput | JackPortIsPhysical);
}

void jackdaw_engine_free_ports(const char **ports)
{
    if (ports)
        jack_free((void *)ports);
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
 * Loop region / record mode
 * ----------------------------------------------------------------------- */

void jackdaw_engine_set_loop_range(off_t start, off_t end)
{
    if (end < start) {
        off_t tmp = start;
        start = end;
        end = tmp;
    }
    if (start < 0)
        start = 0;
    if (end < 0)
        end = 0;
    /* Publish start first, then end, so the RT path never sees end < start. */
    engine.loop_start = start;
    engine.loop_end = end;
}

void jackdaw_engine_get_loop_range(off_t *start, off_t *end)
{
    if (start)
        *start = engine.loop_start;
    if (end)
        *end = engine.loop_end;
}

void jackdaw_engine_set_loop_enabled(gboolean on)
{
    g_atomic_int_set(&engine.loop_enabled, on ? 1 : 0);
}

gboolean jackdaw_engine_get_loop_enabled(void)
{
    return g_atomic_int_get(&engine.loop_enabled) != 0;
}

gboolean jackdaw_engine_has_loop_region(void)
{
    return engine.loop_end > engine.loop_start;
}

void jackdaw_engine_set_record_mode(int mode)
{
    g_atomic_int_set(&engine.record_mode,
                     mode == RECORD_MODE_PUNCH ? RECORD_MODE_PUNCH : RECORD_MODE_NORMAL);
}

int jackdaw_engine_get_record_mode(void)
{
    return g_atomic_int_get(&engine.record_mode);
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
