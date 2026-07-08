#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <sndfile.h>
#include <samplerate.h>

#include "rt_denormal.h" /* FTZ/DAZ for the RT thread */

#include "jackdaw-engine.h"
#include "settings.h"
#include "message.h"

/* -----------------------------------------------------------------------
 * Internal state
 *
 * Ported from the Linux JackDAW engine: transport flags + play_pos, count-in,
 * metronome, stereo master outs, per-track capture ports, loop wrap, and the
 * playback feeder thread that streams clip regions into the RT drain. The
 * recorder thread, MIDI ports, plugin graph and render taps of the original
 * return in later phases; their state fields keep their slots so that code
 * drops back in unchanged.
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

/* -----------------------------------------------------------------------
 * Playback feeder thread
 *
 * A dedicated pthread fills each track's play_buf_L/R ringbuffers from its
 * AudioClip regions so the RT callback always has audio to drain. This is NOT
 * an RT thread: it may allocate, open files (libsndfile) and resample
 * (libsamplerate); the only contract it shares with the RT callback is the
 * lock-free jack_ringbuffer. Ported from the Linux JackDAW engine; the
 * HAVE_SAMPLERATE guards are dropped since libsamplerate is always present on
 * the Haiku target.
 * ----------------------------------------------------------------------- */

/* Output frames produced per slot per inner-loop pass (controls granularity). */
#define FEEDER_CHUNK_FRAMES 2048
/* Raw interleaved frames for one read (covers up to 6:1 downsample). */
#define FEEDER_RAW_FRAMES (FEEDER_CHUNK_FRAMES * 6)
/* Maximum clip channels we deinterleave (ch 0 and 1 are used). */
#define FEEDER_MAX_CHANNELS 8

/* Per-slot feeder state — only the atomic locate fields are shared with the
 * main thread; everything else is private to the feeder thread. */
typedef struct {
    SNDFILE *sf;      /* open file for the current region; NULL = none */
    int open_clip_sr; /* sample rate of the open file */
    int open_clip_ch; /* channel count of the open file */
    SRC_STATE *src_L; /* resampler for channel 0; NULL = no SRC */
    SRC_STATE *src_R; /* resampler for channel 1; NULL = mono or no SRC */
    /* Locate protocol: main thread writes locate_frame then sets locate_req=1.
     * Feeder does CAS(1->0) and applies the seek. */
    volatile gint locate_req;
    off_t locate_frame;       /* timeline frame target; valid when locate_req==1 */
    off_t play_frame;         /* feeder's current timeline frame position */
    ClipRegionSnapshot *snap; /* snapshot the feeder currently holds a ref to */
    int open_region;          /* index into snap of the open region; -1 = none */
} FeederSlot;

static FeederSlot feeder_slots[JACKDAW_MAX_TRACKS];
static pthread_t feeder_tid;
static volatile int feeder_stop_flag;
static gboolean feeder_started = FALSE;

/* Scratch buffers owned exclusively by the feeder thread */
static float *feeder_raw;  /* FEEDER_RAW_FRAMES * FEEDER_MAX_CHANNELS interleaved */
static float *feeder_mono; /* FEEDER_RAW_FRAMES — one deinterleaved channel pre-SRC */
static float *feeder_L;    /* FEEDER_CHUNK_FRAMES — output left channel */
static float *feeder_R;    /* FEEDER_CHUNK_FRAMES — output right channel */

/* Close a slot's open file and SRC states (feeder thread or stop path only).
 * Leaves play_frame and the held snapshot untouched. */
static void feeder_slot_close(guint i)
{
    if (feeder_slots[i].sf) {
        sf_close(feeder_slots[i].sf);
        feeder_slots[i].sf = NULL;
    }
    if (feeder_slots[i].src_L) {
        src_delete(feeder_slots[i].src_L);
        feeder_slots[i].src_L = NULL;
    }
    if (feeder_slots[i].src_R) {
        src_delete(feeder_slots[i].src_R);
        feeder_slots[i].src_R = NULL;
    }
    feeder_slots[i].open_region = -1;
}

/* Full release: file handle, SRC, and snapshot reference. */
static void feeder_slot_release(guint i)
{
    feeder_slot_close(i);
    if (feeder_slots[i].snap) {
        clip_region_snapshot_unref(feeder_slots[i].snap);
        feeder_slots[i].snap = NULL;
    }
    feeder_slots[i].play_frame = 0;
    g_atomic_int_set(&feeder_slots[i].locate_req, 0);
}

/* Write n frames of feeder_L/feeder_R into a track's playback ringbuffers. */
static void feeder_write(JackDawTrack *t, size_t n)
{
    if (n == 0)
        return;
    jack_ringbuffer_write(t->play_buf_L, (const char *)feeder_L, n * sizeof(float));
    jack_ringbuffer_write(t->play_buf_R, (const char *)feeder_R, n * sizeof(float));
}

static void feeder_emit_silence(JackDawTrack *t, size_t n)
{
    memset(feeder_L, 0, n * sizeof(float));
    memset(feeder_R, 0, n * sizeof(float));
    feeder_write(t, n);
}

static void *feeder_thread_func(void *arg)
{
    (void)arg;
    guint i;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000}; /* 2 ms sleep */

    while (!feeder_stop_flag) {
        nanosleep(&ts, NULL);

        if (!(g_atomic_int_get(&engine.transport_flags) & ENGINE_PLAYING))
            continue;
        if (!engine.client)
            continue;

        int jack_sr = (int)jack_get_sample_rate(engine.client);

        for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
            JackDawTrack *t = engine.slots[i];

            if (!t) {
                if (feeder_slots[i].snap || feeder_slots[i].sf)
                    feeder_slot_release(i);
                continue;
            }
            if (!t->play_buf_L || !t->play_buf_R)
                continue;

            /* --- Refresh the immutable region snapshot --- */
            ClipRegionSnapshot *ns = jackdaw_track_ref_snapshot(t);
            if (ns != feeder_slots[i].snap) {
                feeder_slot_close(i);
                if (feeder_slots[i].snap)
                    clip_region_snapshot_unref(feeder_slots[i].snap);
                feeder_slots[i].snap = ns;
            } else {
                clip_region_snapshot_unref(ns);
            }
            ClipRegionSnapshot *snap = feeder_slots[i].snap;

            /* --- Handle locate request from the main thread --- */
            if (g_atomic_int_compare_and_exchange(&feeder_slots[i].locate_req, 1, 0)) {
                feeder_slots[i].play_frame = feeder_slots[i].locate_frame;
                feeder_slot_close(i);
            }

            /* --- Inner fill loop: top up the ringbuffer each wakeup --- */
            while (TRUE) {
                size_t space_L = jack_ringbuffer_write_space(t->play_buf_L) / sizeof(float);
                size_t space_R = jack_ringbuffer_write_space(t->play_buf_R) / sizeof(float);
                size_t out_want = (space_L < space_R) ? space_L : space_R;
                if (out_want > FEEDER_CHUNK_FRAMES)
                    out_want = FEEDER_CHUNK_FRAMES;
                if (out_want == 0)
                    break; /* buffer full */

                off_t pf = feeder_slots[i].play_frame;

                /* --- Loop wrap: keep the feeder's stream inside the loop region
                 * so the ringbuffer never buffers audio past loop_end. --- */
                if (g_atomic_int_get(&engine.loop_enabled)) {
                    off_t l_start = engine.loop_start;
                    off_t l_end = engine.loop_end;
                    if (l_end > l_start) {
                        if (pf == l_end) {
                            pf = l_start;
                            feeder_slots[i].play_frame = pf;
                            feeder_slot_close(i); /* force re-seek of source */
                        }
                        if (pf < l_start) {
                            off_t room = l_start - pf; /* stop at region start */
                            if ((off_t)out_want > room)
                                out_want = (size_t)room;
                        } else if (pf < l_end) {
                            off_t room = l_end - pf; /* stop at region end */
                            if ((off_t)out_want > room)
                                out_want = (size_t)room;
                        }
                    }
                }

                /* Locate the region covering pf and the next region after it. */
                ClipRegion *reg = NULL;
                int reg_idx = -1;
                off_t next_start = -1;
                for (int k = 0; snap && k < snap->n; k++) {
                    ClipRegion *r = &snap->r[k];
                    if (pf >= r->tl_pos && pf < r->tl_pos + r->length) {
                        reg = r;
                        reg_idx = k;
                        break;
                    }
                    if (r->tl_pos > pf && (next_start < 0 || r->tl_pos < next_start))
                        next_start = r->tl_pos;
                }

                /* --- Gap / before first / past end: emit silence --- */
                if (!reg) {
                    size_t sil = out_want;
                    if (next_start >= 0) {
                        off_t to_next = next_start - pf;
                        if (to_next > 0 && (off_t)sil > to_next)
                            sil = (size_t)to_next;
                    }
                    if (sil == 0)
                        break;
                    if (feeder_slots[i].sf)
                        feeder_slot_close(i);
                    feeder_emit_silence(t, sil);
                    feeder_slots[i].play_frame = pf + (off_t)sil;
                    continue;
                }

                int clip_sr = reg->clip ? reg->clip->info.samplerate : jack_sr;
                int clip_ch = reg->clip ? reg->clip->info.channels : 1;
                int eff_ch = (clip_ch > FEEDER_MAX_CHANNELS) ? FEEDER_MAX_CHANNELS : clip_ch;
                gboolean needs_src = (clip_sr != jack_sr);
                off_t d = pf - reg->tl_pos;         /* timeline frames in */
                off_t reg_remain = reg->length - d; /* timeline frames left */
                if (reg_remain <= 0) {
                    feeder_slots[i].play_frame = reg->tl_pos + reg->length;
                    continue;
                }

                /* --- Open / seek file for this region if needed --- */
                if (reg_idx != feeder_slots[i].open_region || !feeder_slots[i].sf) {
                    feeder_slot_close(i);
                    SF_INFO sfi = {0};
                    SNDFILE *sf = reg->clip ? sf_open(reg->clip->path, SFM_READ, &sfi) : NULL;
                    if (!sf) {
                        /* Cannot read — render the rest of the region as silence */
                        size_t sil = out_want;
                        if ((off_t)sil > reg_remain)
                            sil = (size_t)reg_remain;
                        feeder_emit_silence(t, sil);
                        feeder_slots[i].play_frame = pf + (off_t)sil;
                        continue;
                    }
                    off_t file_off =
                        reg->file_in +
                        ((clip_sr == jack_sr) ? d : (off_t)((double)d * clip_sr / jack_sr + 0.5));
                    sf_seek(sf, file_off, SEEK_SET);
                    feeder_slots[i].sf = sf;
                    feeder_slots[i].open_clip_sr = clip_sr;
                    feeder_slots[i].open_clip_ch = clip_ch;
                    feeder_slots[i].open_region = reg_idx;
                    if (needs_src) {
                        int e = 0;
                        feeder_slots[i].src_L = src_new(SRC_SINC_FASTEST, 1, &e);
                        if (eff_ch > 1)
                            feeder_slots[i].src_R = src_new(SRC_SINC_FASTEST, 1, &e);
                    }
                }

                size_t want = out_want;
                if ((off_t)want > reg_remain)
                    want = (size_t)reg_remain;
                gfloat gain = reg->gain;

                if (!needs_src) {
                    /* ---- Direct copy path ---- */
                    sf_count_t got =
                        sf_readf_float(feeder_slots[i].sf, feeder_raw, (sf_count_t)want);
                    if (got < 0)
                        got = 0;
                    if (eff_ch == 1) {
                        for (sf_count_t f = 0; f < got; f++)
                            feeder_L[f] = feeder_R[f] = feeder_raw[f] * gain;
                    } else {
                        for (sf_count_t f = 0; f < got; f++) {
                            feeder_L[f] = feeder_raw[f * eff_ch] * gain;
                            feeder_R[f] = feeder_raw[f * eff_ch + 1] * gain;
                        }
                    }
                    if ((size_t)got < want) {
                        size_t pad = want - (size_t)got;
                        memset(feeder_L + got, 0, pad * sizeof(float));
                        memset(feeder_R + got, 0, pad * sizeof(float));
                    }
                    feeder_write(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;
                } else if (feeder_slots[i].src_L) {
                    /* ---- Resampled path (clip SR != JACK SR) ---- */
                    double ratio = (double)jack_sr / (double)clip_sr;
                    long want_l = (long)want;
                    long in_need = (long)ceil((double)want / ratio) + 8;
                    if (in_need > FEEDER_RAW_FRAMES)
                        in_need = FEEDER_RAW_FRAMES;
                    int eoi = (want == (size_t)reg_remain);

                    sf_count_t got =
                        sf_readf_float(feeder_slots[i].sf, feeder_raw, (sf_count_t)in_need);
                    if (got < 0)
                        got = 0;

                    for (sf_count_t f = 0; f < got; f++)
                        feeder_mono[f] = feeder_raw[f * eff_ch];
                    SRC_DATA sd_L = {.data_in = feeder_mono,
                                     .data_out = feeder_L,
                                     .input_frames = (long)got,
                                     .output_frames = want_l,
                                     .src_ratio = ratio,
                                     .end_of_input = eoi};
                    src_process(feeder_slots[i].src_L, &sd_L);
                    long out_gen = sd_L.output_frames_gen;

                    if (eff_ch > 1 && feeder_slots[i].src_R) {
                        for (sf_count_t f = 0; f < got; f++)
                            feeder_mono[f] = feeder_raw[f * eff_ch + 1];
                        SRC_DATA sd_R = {.data_in = feeder_mono,
                                         .data_out = feeder_R,
                                         .input_frames = (long)got,
                                         .output_frames = want_l,
                                         .src_ratio = ratio,
                                         .end_of_input = eoi};
                        src_process(feeder_slots[i].src_R, &sd_R);
                    } else if (out_gen > 0) {
                        memcpy(feeder_R, feeder_L, (size_t)out_gen * sizeof(float));
                    }

                    for (long k = 0; k < out_gen; k++) {
                        feeder_L[k] *= gain;
                        feeder_R[k] *= gain;
                    }
                    if (out_gen < want_l) {
                        size_t pad = (size_t)(want_l - out_gen);
                        memset(feeder_L + out_gen, 0, pad * sizeof(float));
                        memset(feeder_R + out_gen, 0, pad * sizeof(float));
                    }
                    feeder_write(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;

                    /* Rewind file input SRC did not consume so the next read
                     * stays aligned with the timeline. */
                    long used = sd_L.input_frames_used;
                    if (used < got)
                        sf_seek(feeder_slots[i].sf, -(sf_count_t)(got - used), SEEK_CUR);
                } else {
                    /* SRC needed but its allocation failed — silence the region. */
                    feeder_emit_silence(t, want);
                    feeder_slots[i].play_frame = pf + (off_t)want;
                }
            } /* inner fill loop */
        } /* for each slot */
    } /* while !feeder_stop_flag */

    /* Clean up all open handles and snapshots on thread exit */
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slot_release(i);

    return NULL;
}

static void feeder_start(void)
{
    if (feeder_started)
        return;

    feeder_raw = g_new(float, FEEDER_RAW_FRAMES *FEEDER_MAX_CHANNELS);
    feeder_mono = g_new(float, FEEDER_RAW_FRAMES);
    feeder_L = g_new(float, FEEDER_CHUNK_FRAMES);
    feeder_R = g_new(float, FEEDER_CHUNK_FRAMES);

    feeder_stop_flag = 0;
    memset(feeder_slots, 0, sizeof(feeder_slots));
    for (guint i = 0; i < JACKDAW_MAX_TRACKS; i++)
        feeder_slots[i].open_region = -1;

    if (pthread_create(&feeder_tid, NULL, feeder_thread_func, NULL) != 0) {
        g_free(feeder_raw);
        g_free(feeder_mono);
        g_free(feeder_L);
        g_free(feeder_R);
        feeder_raw = feeder_mono = feeder_L = feeder_R = NULL;
        return;
    }
    feeder_started = TRUE;
}

static void feeder_stop(void)
{
    if (!feeder_started)
        return;
    feeder_stop_flag = 1;
    pthread_join(feeder_tid, NULL);
    feeder_started = FALSE;
    g_free(feeder_raw);
    feeder_raw = NULL;
    g_free(feeder_mono);
    feeder_mono = NULL;
    g_free(feeder_L);
    feeder_L = NULL;
    g_free(feeder_R);
    feeder_R = NULL;
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

        /* Clip playback: drain the feeder's ringbuffers for this track. An
         * instrument track has no audio regions; a stopped transport plays
         * nothing. Short reads (feeder underrun) are zero-padded. Live input
         * monitoring sums on top below. */
        size_t want = nframes * sizeof(float);
        if (!jackdaw_track_is_instrument(t) && t->play_buf_L && t->play_buf_R &&
            (flags & ENGINE_PLAYING)) {
            size_t got_L = jack_ringbuffer_read(t->play_buf_L, (char *)bL, want);
            size_t got_R = jack_ringbuffer_read(t->play_buf_R, (char *)bR, want);
            if (got_L < want)
                memset((char *)bL + got_L, 0, want - got_L);
            if (got_R < want)
                memset((char *)bR + got_R, 0, want - got_R);
        } else {
            memset(bL, 0, want);
            memset(bR, 0, want);
        }

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
    if (engine.project && engine.project->master_muted)
        master_vol = 0.0f;
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

    /* Start the playback feeder thread (fills per-track play ringbuffers). */
    feeder_start();

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

    /* Stop the feeder before tearing down the client so it never touches a
     * closing JACK client or freed ringbuffers. */
    feeder_stop();

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

    /* Allocate the playback ringbuffers (2 seconds at the detected sample rate);
     * the feeder writes them, the RT callback drains them. mlock keeps their
     * pages resident. Sized by sample rate, not buffer size, so a buffer-size
     * change never needs to touch them. */
    {
        jack_nframes_t sr = engine.client ? jack_get_sample_rate(engine.client) : 48000;
        size_t rb_bytes = (size_t)(2 * sr) * sizeof(float);
        track->play_buf_L = jack_ringbuffer_create(rb_bytes);
        track->play_buf_R = jack_ringbuffer_create(rb_bytes);
        if (!track->play_buf_L || !track->play_buf_R) {
            jackdaw_error("jackdaw: ringbuffer allocation failed");
            if (track->play_buf_L)
                jack_ringbuffer_free(track->play_buf_L);
            if (track->play_buf_R)
                jack_ringbuffer_free(track->play_buf_R);
            track->play_buf_L = track->play_buf_R = NULL;
            return TRUE;
        }
        jack_ringbuffer_mlock(track->play_buf_L);
        jack_ringbuffer_mlock(track->play_buf_R);
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
    guint i;
    /* Stop first: with ENGINE_PLAYING clear, neither the RT drain nor the feeder
     * touches the play ringbuffers, so resetting them here is race-free. */
    jackdaw_engine_stop_playback();
    jackdaw_engine_stop_recording();
    engine.play_pos = sample;
    for (i = 0; i < JACKDAW_MAX_TRACKS; i++) {
        JackDawTrack *t = engine.slots[i];
        if (!t)
            continue;
        t->played_frames = sample;
        if (t->play_buf_L)
            jack_ringbuffer_reset(t->play_buf_L);
        if (t->play_buf_R)
            jack_ringbuffer_reset(t->play_buf_R);
        /* Tell the feeder to re-seek this slot to the new position. */
        feeder_slots[i].locate_frame = sample;
        g_atomic_int_set(&feeder_slots[i].locate_req, 1);
    }
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
