#ifndef PLUGINHOST_INTERNAL_H_INCLUDED
#define PLUGINHOST_INTERNAL_H_INCLUDED

/* Shared between the plugin-host translation units only: pluginhost.cpp
 * (VST3 backend — needs the Steinberg SDK headers) and pluginhost_lv2.cpp
 * (LV2 backend — needs lilv/zix). The two third-party header sets never meet
 * in one TU; this header carries the common instance front-struct, the
 * cross-TU backend entry points, and small helpers both backends share.
 * UI code must keep including only the plain-C pluginhost.h. */

#include "pluginhost.h"

#include <math.h>
#include <time.h>

struct Vst3Backend; /* defined in pluginhost.cpp */
struct Lv2Backend;  /* defined in pluginhost_lv2.cpp */

/* Format-independent instance front. Exactly one backend pointer is non-NULL,
 * matching `format`. POD: allocated with g_new0, freed by the owning backend's
 * free path. */
struct PluginInstance {
    PluginFormat format;
    char *name;
    char *key;
    char *category;
    gboolean is_instrument;
    volatile gint active;  /* 1 = processing, 0 = bypassed */
    volatile gint mix_q15; /* wet/dry: 0 = fully dry .. 32768 = fully wet */
    double sample_rate;
    int max_block;
    Vst3Backend *b;  /* PH_VST3 only */
    Lv2Backend *lv2; /* PH_LV2 only */

    /* Dry-signal scratch for the wet/dry mix (allocated to max_block). */
    float *dry_L, *dry_R;

    /* Worst-case µs in process() this period (RT writes, diag reader resets). */
    volatile gint64 diag_max_us;

    /* Drum-rack MIDI learn: the UI arms capture (learn_arm=1) then the RT MIDI
     * path records the first note-on pitch it sees into learn_note; the UI polls
     * ph_drum_learn_take_note() to read and reset it. */
    volatile gint learn_arm;  /* 1 = capturing the next note-on */
    volatile gint learn_note; /* captured pitch, or -1 = none yet */
};

/* Speaker-safety net, applied after every plugin process call (both backends,
 * both the effect and instrument paths): a misbehaving plugin must never send
 * NaN/inf or a runaway level to the speakers. Replace non-finite samples,
 * clamp magnitude to ±4.0 (≈ +12 dBFS — only runaway spikes, not hot mixes;
 * the final [-1,1] clip happens at the render/output stage). RT-safe. */
static inline void ph_safety_clamp(float *L, float *R, int nframes)
{
    for (int i = 0; i < nframes; i++) {
        float a = L[i], c = R[i];
        if (!isfinite(a))
            a = 0.0f;
        else if (a > 4.0f)
            a = 4.0f;
        else if (a < -4.0f)
            a = -4.0f;
        if (!isfinite(c))
            c = 0.0f;
        else if (c > 4.0f)
            c = 4.0f;
        else if (c < -4.0f)
            c = -4.0f;
        L[i] = a;
        R[i] = c;
    }
}

/* JACKDAW_DIAG helpers (per-TU static cache is fine — the env never changes
 * mid-run). */
static inline gboolean ph_diag_enabled_i(void)
{
    static int e = -1;
    if (e < 0)
        e = (g_getenv("JACKDAW_DIAG") != NULL) ? 1 : 0;
    return e;
}

static inline gint64 ph_now_us_i(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- LV2 backend entry points (pluginhost_lv2.cpp), called only from the
 * dispatchers in pluginhost.cpp. Threading contracts match the public API:
 * ph_lv2_process is the RT path (no alloc/lock/log); everything else runs on
 * the instance's owning window thread. ---- */

G_BEGIN_DECLS

void ph_lv2_init(double sample_rate, int max_block);
void ph_lv2_shutdown(void);
/* Prepend LV2 catalog entries (PluginInfo*, key = plugin URI) to *catalog. */
void ph_lv2_scan(GList **catalog);
PluginInstance *ph_lv2_instantiate(const PluginInfo *info);
void ph_lv2_free(PluginInstance *inst);
void ph_lv2_process(PluginInstance *inst, float *L, float *R, int nframes);
void ph_lv2_reset(PluginInstance *inst);
guint ph_lv2_param_count(PluginInstance *inst);
void ph_lv2_param_name(PluginInstance *inst, guint i, char *buf, int buflen);
float ph_lv2_param_get(PluginInstance *inst, guint i);
void ph_lv2_param_set(PluginInstance *inst, guint i, float v);
void ph_lv2_param_display(PluginInstance *inst, guint i, char *buf, int buflen);
gboolean ph_lv2_param_is_stepped(PluginInstance *inst, guint i, gint *steps);
gboolean ph_lv2_state_save(PluginInstance *inst, void **out, gsize *out_len);
gboolean ph_lv2_state_load(PluginInstance *inst, const void *data, gsize len);
void *ph_lv2_ui_create(PluginInstance *inst);
void ph_lv2_ui_poll(PluginInstance *inst);
void ph_lv2_ui_destroy(PluginInstance *inst);

G_END_DECLS

#endif /* PLUGINHOST_INTERNAL_H_INCLUDED */
