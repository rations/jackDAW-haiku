#ifndef PLUGINHOST_H_INCLUDED
#define PLUGINHOST_H_INCLUDED

#include <glib.h>

G_BEGIN_DECLS

/*
 * Plugin host — VST3-only port of the Linux JackDAW unified host API subset.
 * Discovery and instantiation happen on a UI thread; pluginhost_process() is
 * RT-safe (no malloc, no locks). Parameter changes travel UI → RT through the
 * SDK's lock-free ParameterChangeTransfer ring, never by touching RT state
 * from the UI thread. The implementation (pluginhost.cpp, C++17) builds on
 * the Haiku-native VST3 SDK port from the sibling VST3-haiku project.
 */

typedef enum { PH_VST3 = 0, PH_NFORMATS } PluginFormat;

/* A catalog entry produced by scanning. key = "<bundle path>\n<class name>"
 * (one .vst3 module can contain many effect classes, e.g. mda-vst3). */
typedef struct {
    PluginFormat format;
    char *key;
    char *name;
    char *category;         /* VST3 subcategory string, e.g. "Fx|Delay" */
    gboolean is_instrument; /* synth/instrument (takes MIDI, makes audio) */
} PluginInfo;

typedef struct PluginInstance PluginInstance;

/* One MIDI event for delivery to an instrument plugin. `time` is the sample
 * offset within the current process block; size is 1..3 bytes. */
typedef struct {
    guint32 time;
    guint8 size;
    guint8 data[3];
} PhMidiEvent;

/* Call once at startup with the engine sample rate / max JACK block. */
void pluginhost_init(double sample_rate, int max_block);
void pluginhost_shutdown(void);

/* `JackDAW --scan-plugin VST3 <path>`: load + describe one plugin bundle in
 * this throwaway process and print its classes to stdout. Call at the very
 * top of main(); if argv matches, exit with the returned code. Keeps
 * arbitrary third-party binaries out of the main process during catalog
 * scans (a crashing module kills only the helper). */
int pluginhost_scan_helper_main(int argc, char **argv);

/* Catalog (scans lazily on first call). GList of PluginInfo* (borrowed).
 * Bundles are found in the Haiku VST3 add-on directories (find_paths) plus
 * $HOME/.vst3; each is described out-of-process via the scan helper. */
const GList *pluginhost_catalog(void);
void pluginhost_rescan(void);

/* Lifecycle. Instantiate and free from the SAME thread that will make all
 * other non-RT calls on the instance (its FX window's looper thread). */
PluginInstance *pluginhost_instantiate(const PluginInfo *info);
void pluginhost_free(PluginInstance *inst);

/* RT processing — in-place stereo. Does nothing when bypassed. Re-arms
 * FTZ/DAZ and clamps non-finite/runaway output (speaker safety net). */
void pluginhost_process(PluginInstance *inst, float *L, float *R, int nframes);

/* RT processing for an INSTRUMENT: deliver this block's MIDI events then
 * render audio into L/R (which the caller has pre-filled, typically with
 * silence). Same safety net as pluginhost_process. */
void pluginhost_process_midi(PluginInstance *inst, const PhMidiEvent *ev, int n_ev, float *L,
                             float *R, int nframes);

/* TRUE if this plugin is an instrument (synth) rather than an audio effect. */
gboolean pluginhost_is_instrument(PluginInstance *inst);

/* Publish transport state for plugins that query host time (tempo/position
 * in the VST3 ProcessContext). Called from the RT thread each block. */
void pluginhost_set_transport(double bpm, double sr, gint64 frame, gboolean playing);

/* Clear internal DSP state (reverb/delay tails) without touching parameters.
 * NOT RT-safe: only while the RT thread is not processing this instance. */
void pluginhost_reset(PluginInstance *inst);

/* Opaque full-state save/restore for project save/reload: the component and
 * controller state streams (captures loaded .nam/.wav paths and everything
 * else the parameter list misses). state_save g_malloc's *out (caller frees).
 * Both main-thread only. */
gboolean pluginhost_state_save(PluginInstance *inst, void **out, gsize *out_len);
gboolean pluginhost_state_load(PluginInstance *inst, const void *data, gsize len);

/* Bypass (atomic; read in the RT thread). */
void pluginhost_set_active(PluginInstance *inst, gboolean on);
gboolean pluginhost_is_active(PluginInstance *inst);

/* Wet/dry mix in [0,1]: 0 = fully dry, 1 = fully wet (default). Atomic. */
void pluginhost_set_mix(PluginInstance *inst, float mix);
float pluginhost_get_mix(PluginInstance *inst);

/* Identity for project save/reload (recreate via PluginInfo + instantiate). */
const char *pluginhost_name(PluginInstance *inst);
PluginFormat pluginhost_format(PluginInstance *inst);
const char *pluginhost_key(PluginInstance *inst);
const char *pluginhost_category(PluginInstance *inst);

/* Generic parameter access (drives the FX window sliders). Values are VST3
 * NORMALIZED [0,1]; param_display formats the human-readable value+units
 * (e.g. "185.0 ms"). Call from the instance's window thread only. */
guint pluginhost_param_count(PluginInstance *inst);
void pluginhost_param_name(PluginInstance *inst, guint i, char *buf, int buflen);
float pluginhost_param_get(PluginInstance *inst, guint i);
void pluginhost_param_set(PluginInstance *inst, guint i, float v);
void pluginhost_param_display(PluginInstance *inst, guint i, char *buf, int buflen);
/* TRUE if parameter i is a stepped list (kIsList / stepCount) — shown as a
 * cycling control rather than a slider. *steps = number of steps (>= 1). */
gboolean pluginhost_param_is_stepped(PluginInstance *inst, guint i, gint *steps);

/* File loading without a plugin GUI (the INamFileLoader host extension,
 * discovered via queryInterface — NAMku implements it; plug-ins that don't
 * simply report no loader and get no file buttons). which: 0 = model (.nam),
 * 1 = impulse response (.wav). */
enum { PH_FILE_MODEL = 0, PH_FILE_IR = 1 };
gboolean pluginhost_has_file_loader(PluginInstance *inst);
gboolean pluginhost_file_get(PluginInstance *inst, int which, char *buf, int buflen);
gboolean pluginhost_file_set(PluginInstance *inst, int which, const char *path);

/* --- Diagnostics (JACKDAW_DIAG) --- */
/* Mark the calling thread as inside/outside the JACK process callback so the
 * host context can detect plug-ins that allocate messages on the RT thread. */
void ph_rt_mark(int on);
int ph_rt_active(void);
/* IHostApplication::createInstance calls made on the RT thread (each one is
 * a heap allocation in the audio callback — must stay 0 in steady state). */
guint64 ph_vst3_rt_alloc_count(void);
/* Read-and-reset the worst-case µs spent in this plugin's process(). */
gint64 pluginhost_diag_take_max_us(PluginInstance *inst);

G_END_DECLS

#endif /* PLUGINHOST_H_INCLUDED */
