/* pluginhost_vst2.cpp — VST2 backend for the jackDAW plugin host.
 *
 * Adapted from the Linux JackDAW VST2 host, reshaped to the Haiku port's
 * front-struct backend model (one PluginInstance, a Vst2Backend behind it,
 * dispatched from pluginhost.cpp on inst->format). Drives the plain VST 2.4
 * `AEffect` ABI declared in the clean-room vst2_abi.h — no SDK, no GTK/X11.
 *
 * There are no native Haiku VST2 plug-ins; the real targets are Windows .dll
 * plug-ins bridged by vstbridge, which installs a native Haiku .so stub per
 * plug-in under ~/.vst. That stub exports VSTPluginMain and returns an ordinary
 * AEffect that marshals every call to a Wine host — so from here a bridged VST2
 * plug-in loads and runs exactly like a local one; Wine is invisible below the
 * ABI. Only the plug-in editor is Haiku-specific (a captured shared-memory
 * surface, not an embedded window); that path is enabled separately once the
 * vstbridge VST2 bridge publishes the surface (see ph_vst2_ui_create).
 *
 * Threading: scan / instantiate / free / params / state run on the instance's
 * owning window looper; ph_vst2_process[_midi] run on the JACK RT thread and
 * never allocate, lock, or log. The MIDI event block and channel buffers are
 * pre-allocated at instantiate.
 */

#include "vst2_abi.h"

#include "pluginhost_internal.h"

#include "../engine/rt_denormal.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Interface Kit — the editor host view (BView) embedded in the FX window.
 * Safe here: vst2_abi.h is the plain AEffect ABI (stdint types), so unlike the
 * VST3 SDK it does not collide with SupportDefs.h's int8/int16 typedefs. */
#include <MessageRunner.h>
#include <Messenger.h>
#include <View.h>
#include <Window.h>

/* Worst-case events delivered to an instrument in one block (pre-allocated). */
#define VST2_MAX_EVENTS 1024

/* Engine sample rate / max JACK block, published at init and answered to
 * plug-ins through the host callback. */
static double vst2_sr = 48000.0;
static int vst2_maxblock = 1024;

class Vst2EditorHostView; /* defined below, before ph_vst2_ui_create */

struct Vst2Backend {
    void *dl;
    AEffect *eff;

    /* Channel buffers sized to the plug-in's ACTUAL port counts. processReplacing
     * writes every numOutputs channel, so a fixed [2] array would overrun for a
     * multi-out plug-in and let it scribble past our buffers. */
    int n_in, n_out;
    float **in_ptrs;  /* [max(n_in,1)]  channel pointers handed to the plug-in */
    float **out_ptrs; /* [max(n_out,1)] channel pointers handed to the plug-in */
    float **out_bufs; /* [n_out] backing scratch the plug-in fills */
    float *silence;   /* zeroed input, shared read-only (max_block) */
    int max_block;
    gboolean is_synth;

    /* Pre-allocated VST event block for MIDI delivery (no RT malloc). */
    VstMidiEvent *midi_pool; /* VST2_MAX_EVENTS entries */
    VstEvents *vst_events;   /* header + VST2_MAX_EVENTS event pointers */

    /* Native editor host view, created once by ph_vst2_ui_create and deleted by
     * ph_vst2_ui_destroy. NULL when the plug-in has no editor / none created. */
    Vst2EditorHostView *editor_view;
};

/* Path validation before loading plug-in code: absolute, no "..", sane length.
 * A local copy — this TU is kept independent of pluginhost.cpp's helpers. */
static gboolean vst2_path_is_safe(const char *path)
{
    if (!path || path[0] != '/')
        return FALSE;
    size_t len = strlen(path);
    if (len < 5 || len > 4000) /* shortest plausible: "/a.so" */
        return FALSE;
    if (strstr(path, ".."))
        return FALSE;
    return TRUE;
}

/* ---- Host callback ----
 * Instruments (drum samplers, synths) query host time via audioMasterGetTime
 * and announce MIDI via canDo — answer those so they run. Reached from the RT
 * thread during process; the VstTimeInfo is thread_local so parallel track
 * workers never share it (the engine processes tracks on a work-stealing pool,
 * so two plug-ins can query time at once). RT-safe: no alloc/lock/log. */
static intptr_t VST_CALL_CONV vst2_master(AEffect *e, int32_t op, int32_t idx, intptr_t val,
                                          void *ptr, float opt)
{
    (void)e;
    (void)idx;
    (void)val;
    (void)opt;
    switch (op) {
        case audioMasterVersion:
            return 2400;
        case audioMasterGetTime: {
            static thread_local VstTimeInfo vti;
            double bpm = 120.0, sr = vst2_sr;
            gint64 frame = 0;
            gboolean playing = FALSE;
            ph_get_transport(&bpm, &sr, &frame, &playing);
            memset(&vti, 0, sizeof vti);
            vti.sampleRate = sr;
            vti.samplePos = (double)frame;
            vti.tempo = bpm;
            double fpb = (bpm > 0.0) ? sr * 60.0 / bpm : 0.0; /* frames per beat */
            vti.ppqPos = (fpb > 0.0) ? (double)frame / fpb : 0.0;
            vti.timeSigNumerator = 4;
            vti.timeSigDenominator = 4;
            vti.flags = kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid;
            if (playing)
                vti.flags |= kVstTransportPlaying;
            return (intptr_t)&vti;
        }
        case audioMasterGetSampleRate: {
            double sr = vst2_sr;
            ph_get_transport(NULL, &sr, NULL, NULL);
            return (intptr_t)sr;
        }
        case audioMasterGetBlockSize:
            return (intptr_t)vst2_maxblock;
        case audioMasterWantMidi:
            return 1;
        case audioMasterCurrentId:
            return 0;
        case audioMasterGetCurrentProcessLevel:
            return 2; /* realtime */
        case audioMasterCanDo:
            if (ptr && (!strcmp((const char *)ptr, "sendVstMidiEvent") ||
                        !strcmp((const char *)ptr, "receiveVstMidiEvent") ||
                        !strcmp((const char *)ptr, "sendVstTimeInfo")))
                return 1;
            return 0;
        default:
            return 0;
    }
}

/* ---- Load ---- */

static AEffect *vst2_load(const char *path, void **dl_out)
{
    if (!vst2_path_is_safe(path))
        return NULL;
    /* dlopen fails cleanly on a non-native / foreign ELF (e.g. a Linux .so),
     * so only real Haiku plug-in stubs get this far. */
    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl)
        return NULL;
    Vst2EntryProc entry = (Vst2EntryProc)dlsym(dl, "VSTPluginMain");
    if (!entry)
        entry = (Vst2EntryProc)dlsym(dl, "main");
    if (!entry) {
        dlclose(dl);
        return NULL;
    }
    AEffect *eff = entry(vst2_master);
    if (!eff || eff->magic != kEffectMagic) {
        dlclose(dl);
        return NULL;
    }
    *dl_out = dl;
    return eff;
}

/* ---- Init / shutdown ---- */

extern "C" void ph_vst2_init(double sample_rate, int max_block)
{
    vst2_sr = sample_rate;
    vst2_maxblock = max_block > 64 ? max_block : 64;
}

extern "C" void ph_vst2_shutdown(void)
{
}

/* ---- Scan ---- */

static gboolean vst2_is_synth(AEffect *eff)
{
    return (eff->flags & effFlagsIsSynth) ||
           eff->dispatcher(eff, effGetPlugCategory, 0, 0, NULL, 0.0f) == kPlugCategSynth;
}

/* Helper-process only: load + describe one .so and print its catalog line.
 * A crashing static initializer takes down only the throwaway helper. */
extern "C" void ph_vst2_describe(const char *path)
{
    void *dl = NULL;
    AEffect *eff = vst2_load(path, &dl);
    if (!eff)
        return;
    char nm[128] = {0};
    eff->dispatcher(eff, effGetEffectName, 0, 0, nm, 0.0f);
    if (!nm[0]) {
        gchar *b = g_path_get_basename(path);
        g_strlcpy(nm, b, sizeof(nm));
        g_free(b);
    }
    gboolean synth = vst2_is_synth(eff);
    printf("VST2\t%s\t%s\n", nm, synth ? "Instrument|VST2" : "VST2");
    eff->dispatcher(eff, effClose, 0, 0, NULL, 0.0f);
    if (dl)
        dlclose(dl);
}

static void vst2_scan_dir(const char *dir, GList **catalog, int depth)
{
    if (depth > 6)
        return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d)
        return;
    const char *e;
    while ((e = g_dir_read_name(d))) {
        gchar *full = g_build_filename(dir, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR))
            vst2_scan_dir(full, catalog, depth + 1);
        else if (g_str_has_suffix(e, ".so"))
            ph_scan_via_helper("VST2", full, catalog); /* out-of-process describe */
        g_free(full);
    }
    g_dir_close(d);
}

extern "C" void ph_vst2_scan(GList **catalog)
{
    /* User-supplied search list first. */
    const char *envp = g_getenv("VST_PATH");
    if (envp) {
        gchar **parts = g_strsplit(envp, ":", -1);
        for (gchar **p = parts; *p; p++)
            if (**p)
                vst2_scan_dir(*p, catalog, 0);
        g_strfreev(parts);
    }
    /* vstbridge installs its native .so stubs here (vstbridgectl sync). */
    gchar *vst = g_build_filename(g_get_home_dir(), ".vst", NULL);
    vst2_scan_dir(vst, catalog, 0);
    g_free(vst);
}

/* ---- Process ----
 * Run processReplacing once. Bind EVERY channel the plug-in expects: ch0=inL,
 * ch1=inR, the rest silence; each output channel gets its own scratch (the
 * plug-in writes all n_out), then out[0]/out[1] are copied back to L/R. */
static void vst2_run(Vst2Backend *b, float *inL, float *inR, float *L, float *R, int n)
{
    AEffect *e = b->eff;
    if (!e->processReplacing)
        return;
    for (int i = 0; i < b->n_in; i++)
        b->in_ptrs[i] = (i == 0) ? inL : (i == 1) ? inR : b->silence;
    for (int i = 0; i < b->n_out; i++)
        b->out_ptrs[i] = b->out_bufs[i];
    e->processReplacing(e, b->in_ptrs, b->out_ptrs, n);
    if (b->n_out > 0) {
        memcpy(L, b->out_bufs[0], (size_t)n * sizeof(float));
        memcpy(R, b->out_bufs[b->n_out > 1 ? 1 : 0], (size_t)n * sizeof(float));
    }
}

extern "C" void ph_vst2_process(PluginInstance *inst, float *L, float *R, int nframes)
{
    if (!inst || !inst->vst2)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave L/R as-is */
    Vst2Backend *b = inst->vst2;
    if (nframes > b->max_block)
        return;

    int mq = g_atomic_int_get(&inst->mix_q15);
    gboolean blend = (mq < 32768) && inst->dry_L && inst->dry_R;
    if (blend) {
        memcpy(inst->dry_L, L, sizeof(float) * (size_t)nframes);
        memcpy(inst->dry_R, R, sizeof(float) * (size_t)nframes);
    }

    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled_i() ? ph_now_us_i() : 0;

    vst2_run(b, L, R, L, R, nframes); /* effect: in-place audio */

    if (t0) {
        gint64 d = ph_now_us_i() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    ph_safety_clamp(L, R, nframes);

    if (blend) { /* wet/dry crossfade */
        float wet = mq * (1.0f / 32768.0f);
        float dry = 1.0f - wet;
        for (int i = 0; i < nframes; i++) {
            L[i] = inst->dry_L[i] * dry + L[i] * wet;
            R[i] = inst->dry_R[i] * dry + R[i] * wet;
        }
    }
}

extern "C" void ph_vst2_process_midi(PluginInstance *inst, const PhMidiEvent *ev, int n_ev,
                                     float *L, float *R, int nframes)
{
    if (!inst || !inst->vst2)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave the caller's (silent) buffers as-is */
    Vst2Backend *b = inst->vst2;
    if (nframes > b->max_block)
        return;

    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled_i() ? ph_now_us_i() : 0;

    if (n_ev > 0 && b->vst_events) {
        if (n_ev > VST2_MAX_EVENTS)
            n_ev = VST2_MAX_EVENTS;
        for (int i = 0; i < n_ev; i++) {
            VstMidiEvent *me = &b->midi_pool[i];
            memset(me, 0, sizeof *me);
            me->type = kVstMidiType;
            me->byteSize = sizeof(VstMidiEvent);
            me->deltaFrames = (int32_t)ev[i].time;
            for (int k = 0; k < 3 && k < ev[i].size; k++)
                me->midiData[k] = (char)ev[i].data[k];
            b->vst_events->events[i] = (VstEvent *)me;
        }
        b->vst_events->numEvents = n_ev;
        b->eff->dispatcher(b->eff, effProcessEvents, 0, 0, b->vst_events, 0.0f);
    }

    /* Instruments take no audio input — feed silence; effects that also take
     * MIDI get the incoming signal. */
    if (b->is_synth)
        vst2_run(b, b->silence, b->silence, L, R, nframes);
    else
        vst2_run(b, L, R, L, R, nframes);

    if (t0) {
        gint64 d = ph_now_us_i() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    ph_safety_clamp(L, R, nframes);
}

extern "C" void ph_vst2_reset(PluginInstance *inst)
{
    if (!inst || !inst->vst2 || !inst->vst2->eff)
        return;
    AEffect *e = inst->vst2->eff;
    /* Toggle processing + power off/on: clears tails and any held notes. */
    e->dispatcher(e, effStopProcess, 0, 0, NULL, 0.0f);
    e->dispatcher(e, effMainsChanged, 0, 0, NULL, 0.0f);
    e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0.0f);
    e->dispatcher(e, effStartProcess, 0, 0, NULL, 0.0f);
}

/* ---- Parameters (VST2 parameters are already normalized to [0,1]) ---- */

extern "C" guint ph_vst2_param_count(PluginInstance *inst)
{
    return (guint)inst->vst2->eff->numParams;
}

extern "C" void ph_vst2_param_name(PluginInstance *inst, guint i, char *buf, int buflen)
{
    AEffect *e = inst->vst2->eff;
    char tmp[128] = {0};
    e->dispatcher(e, effGetParamName, (int32_t)i, 0, tmp, 0.0f);
    g_strlcpy(buf, tmp[0] ? tmp : "param", buflen);
}

extern "C" float ph_vst2_param_get(PluginInstance *inst, guint i)
{
    AEffect *e = inst->vst2->eff;
    return e->getParameter ? e->getParameter(e, (int32_t)i) : 0.0f;
}

extern "C" void ph_vst2_param_set(PluginInstance *inst, guint i, float v)
{
    AEffect *e = inst->vst2->eff;
    if (v < 0.0f)
        v = 0.0f;
    else if (v > 1.0f)
        v = 1.0f;
    if (e->setParameter)
        e->setParameter(e, (int32_t)i, v);
}

extern "C" void ph_vst2_param_display(PluginInstance *inst, guint i, char *buf, int buflen)
{
    AEffect *e = inst->vst2->eff;
    char val[128] = {0}, unit[64] = {0};
    e->dispatcher(e, effGetParamDisplay, (int32_t)i, 0, val, 0.0f);
    e->dispatcher(e, effGetParamLabel, (int32_t)i, 0, unit, 0.0f);
    if (unit[0])
        g_snprintf(buf, buflen, "%s %s", val, unit);
    else
        g_strlcpy(buf, val, buflen);
}

extern "C" gboolean ph_vst2_param_is_stepped(PluginInstance *inst, guint i, gint *steps)
{
    /* The VST2 ABI exposes no reliable step metadata through the calls we make,
     * so every parameter is a continuous [0,1] slider. */
    (void)inst;
    (void)i;
    if (steps)
        *steps = 0;
    return FALSE;
}

/* ---- Opaque state (project save/reload) ----
 * Plug-ins that advertise effFlagsProgramChunks keep their full state in an
 * opaque bank chunk (effGetChunk/effSetChunk), NOT in the parameter list — this
 * is where loaded sample/IR paths and internal modes live. isPreset=0 means the
 * whole bank (entire plug-in state), which is what we round-trip. */
extern "C" gboolean ph_vst2_state_save(PluginInstance *inst, void **out, gsize *out_len)
{
    AEffect *e = inst->vst2->eff;
    if (!e || !(e->flags & effFlagsProgramChunks))
        return FALSE;
    void *chunk = NULL;
    intptr_t n = e->dispatcher(e, effGetChunk, 0 /* bank */, 0, &chunk, 0.0f);
    if (n <= 0 || !chunk)
        return FALSE;
    *out = g_memdup2(chunk, (gsize)n); /* chunk is plug-in owned; copy it */
    *out_len = (gsize)n;
    return TRUE;
}

extern "C" gboolean ph_vst2_state_load(PluginInstance *inst, const void *data, gsize len)
{
    AEffect *e = inst->vst2->eff;
    if (!e || !(e->flags & effFlagsProgramChunks))
        return FALSE;
    /* effSetChunk consumes ptr synchronously; the const-cast is safe. */
    e->dispatcher(e, effSetChunk, 0 /* bank */, (intptr_t)len, (void *)data, 0.0f);
    return TRUE;
}

/* ---- Native editor ----
 * The vstbridge VST2 bridge publishes the plug-in editor as a captured
 * shared-memory surface and paints it into whatever BView we hand to
 * effEditOpen (matching what its VST3 bridge does). A native VST2 plug-in would
 * embed its own view into that same BView. Either way the FX window AddChild()s
 * the container view below; plug-ins without an editor report so via
 * effFlagsHasEditor and get the generic parameter sliders instead. */

/* Idle tick posted to the editor view ~30 Hz (VST2 hosts must pump effEditIdle;
 * the bridge also runs its own Win32 idle timer, so this is belt-and-braces and
 * carries plug-in-driven resizes through). */
static const uint32 MSG_VST2_EDIT_IDLE = 'v2ei';

/* Container BView the FX window embeds: on attach it opens the plug-in editor
 * with itself as the parent handle (the bridge attaches its painting view as a
 * child / a native plug-in embeds directly), pumps effEditIdle on a runner, and
 * closes the editor on detach. Owned by the backend; deleted in
 * ph_vst2_ui_destroy after the FX window removed it. */
class Vst2EditorHostView : public BView
{
public:
    Vst2EditorHostView(AEffect *eff, BRect frame)
        : BView(frame, "vst2-editor", B_FOLLOW_NONE, 0), fEff(eff)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
        SetExplicitMinSize(BSize(frame.Width(), frame.Height()));
        SetExplicitMaxSize(BSize(frame.Width(), frame.Height()));
        SetExplicitPreferredSize(BSize(frame.Width(), frame.Height()));
    }

    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        if (fEff && !fAttached) {
            /* Hand the plug-in this BView* as its editor parent handle. */
            fEff->dispatcher(fEff, effEditOpen, 0, 0, this, 0.0f);
            fAttached = true;
            if (!fIdle)
                fIdle =
                    new BMessageRunner(BMessenger(this), new BMessage(MSG_VST2_EDIT_IDLE), 33000);
        }
    }

    void DetachedFromWindow() override
    {
        CloseEditor();
        BView::DetachedFromWindow();
    }

    void MessageReceived(BMessage *msg) override
    {
        if (msg->what == MSG_VST2_EDIT_IDLE) {
            if (fEff && fAttached)
                fEff->dispatcher(fEff, effEditIdle, 0, 0, NULL, 0.0f);
            return;
        }
        BView::MessageReceived(msg);
    }

    /* effEditClose + stop the idle runner. Idempotent (detach then destroy both
     * call it); runs before the AEffect is freed. */
    void CloseEditor()
    {
        if (fIdle) {
            delete fIdle;
            fIdle = NULL;
        }
        if (fEff && fAttached) {
            fAttached = false;
            fEff->dispatcher(fEff, effEditClose, 0, 0, NULL, 0.0f);
        }
    }

private:
    AEffect *fEff;
    bool fAttached = false;
    BMessageRunner *fIdle = NULL;
};

extern "C" void *ph_vst2_ui_create(PluginInstance *inst)
{
    if (!inst || !inst->vst2 || !inst->vst2->eff)
        return NULL;
    Vst2Backend *b = inst->vst2;
    if (b->editor_view) /* created once per instance */
        return b->editor_view;
    AEffect *eff = b->eff;
    if (!(eff->flags & effFlagsHasEditor))
        return NULL; /* no editor: FX window shows the generic sliders */

    /* Size the container to the editor up front, before the FX window embeds
     * it. ERect is a pointer-to-pointer out param; a plug-in that reports
     * nothing yet gets a reasonable default (the bridge resizes us later via a
     * plug-in-driven resize). */
    ERect *rect = NULL;
    eff->dispatcher(eff, effEditGetRect, 0, 0, &rect, 0.0f);
    float w = 600.0f, h = 400.0f;
    if (rect && rect->right > rect->left && rect->bottom > rect->top) {
        w = (float)(rect->right - rect->left);
        h = (float)(rect->bottom - rect->top);
    }
    b->editor_view = new Vst2EditorHostView(eff, BRect(0, 0, w - 1, h - 1));
    return b->editor_view;
}

extern "C" void ph_vst2_ui_poll(PluginInstance *inst)
{
    /* Nothing to drain: the bridge's painting view repaints itself from the
     * shared-memory surface on its own timer, and effEditIdle is pumped by the
     * host view's runner. */
    (void)inst;
}

extern "C" void ph_vst2_ui_destroy(PluginInstance *inst)
{
    if (!inst || !inst->vst2 || !inst->vst2->editor_view)
        return;
    Vst2Backend *b = inst->vst2;
    /* The FX window has already removed the view from its parent (which closed
     * the editor via DetachedFromWindow); CloseEditor() again is a no-op. */
    b->editor_view->CloseEditor();
    delete b->editor_view;
    b->editor_view = NULL;
}

/* ---- Instantiate / free ---- */

extern "C" PluginInstance *ph_vst2_instantiate(const PluginInfo *info)
{
    void *dl = NULL;
    AEffect *eff = vst2_load(info->key, &dl);
    if (!eff)
        return NULL;

    Vst2Backend *b = g_new0(Vst2Backend, 1);
    b->dl = dl;
    b->eff = eff;
    b->max_block = vst2_maxblock;
    /* Size channel arrays to the plug-in's real port counts (pointer arrays
     * clamped to >= 1 so those handed to processReplacing are never NULL). */
    b->n_in = eff->numInputs > 0 ? eff->numInputs : 0;
    b->n_out = eff->numOutputs > 0 ? eff->numOutputs : 0;
    b->in_ptrs = g_new0(float *, b->n_in > 0 ? b->n_in : 1);
    b->out_ptrs = g_new0(float *, b->n_out > 0 ? b->n_out : 1);
    b->out_bufs = g_new0(float *, b->n_out > 0 ? b->n_out : 1);
    for (int i = 0; i < b->n_out; i++)
        b->out_bufs[i] = g_new0(float, b->max_block);
    b->silence = g_new0(float, b->max_block);
    b->is_synth = vst2_is_synth(eff);

    /* Pre-allocate the MIDI event block (no malloc in the RT path). VstEvents
     * has a flexible events[1] tail; size it for VST2_MAX_EVENTS pointers. */
    b->midi_pool = g_new0(VstMidiEvent, VST2_MAX_EVENTS);
    b->vst_events =
        (VstEvents *)g_malloc0(sizeof(VstEvents) + (VST2_MAX_EVENTS - 1) * sizeof(VstEvent *));

    eff->dispatcher(eff, effOpen, 0, 0, NULL, 0.0f);
    eff->dispatcher(eff, effSetSampleRate, 0, 0, NULL, (float)vst2_sr);
    eff->dispatcher(eff, effSetBlockSize, 0, b->max_block, NULL, 0.0f);
    eff->dispatcher(eff, effMainsChanged, 0, 1, NULL, 0.0f);
    eff->dispatcher(eff, effStartProcess, 0, 0, NULL, 0.0f);

    char nm[128] = {0};
    eff->dispatcher(eff, effGetEffectName, 0, 0, nm, 0.0f);

    PluginInstance *pi = g_new0(PluginInstance, 1);
    pi->format = PH_VST2;
    pi->name = g_strdup(nm[0] ? nm : (info->name && info->name[0] ? info->name : info->key));
    pi->key = g_strdup(info->key);
    pi->category = g_strdup(info->category ? info->category : "VST2");
    pi->is_instrument = b->is_synth;
    pi->active = 1;
    pi->mix_q15 = 32768;
    pi->sample_rate = vst2_sr;
    pi->max_block = b->max_block;
    pi->dry_L = g_new0(float, b->max_block);
    pi->dry_R = g_new0(float, b->max_block);
    pi->learn_arm = 0;
    pi->learn_note = -1;
    pi->vst2 = b;
    return pi;
}

extern "C" void ph_vst2_free(PluginInstance *inst)
{
    if (!inst)
        return;
    Vst2Backend *b = inst->vst2;
    if (b) {
        /* Defensive: the FX window detaches + destroys the editor before free
         * (so this is normally already NULL). Tear it down here too so a
         * lingering view can never dispatch into a closed AEffect. */
        if (b->editor_view) {
            b->editor_view->CloseEditor();
            delete b->editor_view;
            b->editor_view = NULL;
        }
        if (b->eff) {
            b->eff->dispatcher(b->eff, effStopProcess, 0, 0, NULL, 0.0f);
            b->eff->dispatcher(b->eff, effMainsChanged, 0, 0, NULL, 0.0f);
            b->eff->dispatcher(b->eff, effClose, 0, 0, NULL, 0.0f);
        }
        if (b->dl)
            dlclose(b->dl);
        for (int i = 0; i < b->n_out; i++)
            g_free(b->out_bufs[i]);
        g_free(b->out_bufs);
        g_free(b->out_ptrs);
        g_free(b->in_ptrs);
        g_free(b->silence);
        g_free(b->midi_pool);
        g_free(b->vst_events);
        g_free(b);
    }
    g_free(inst->dry_L);
    g_free(inst->dry_R);
    g_free(inst->name);
    g_free(inst->key);
    g_free(inst->category);
    g_free(inst);
}
