/* pluginhost.cpp — VST3 plugin host for jackDAW-haiku.
 *
 * Port of the Linux JackDAW host (pluginhost.c + pluginhost_vst3.cpp) reduced
 * to the VST3 backend, with every X11/GDK editor path removed: on Haiku no
 * plug-in ships a compatible IPlugView yet, so hosting is parameter-based and
 * the FX window builds its own native controls from the IEditController.
 *
 * Threading: instantiate/free/params/state run on the owning FX window's
 * looper thread; pluginhost_process() runs on the JACK RT thread. UI → RT
 * parameter changes go through the SDK's lock-free ParameterChangeTransfer
 * ring (the Linux original wrote the RT ParameterChanges from the UI thread —
 * fixed here). Nothing on the RT path allocates, locks, or logs.
 */

#include "pluginhost.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include "inamfileloader.h"

#include "engine/rt_denormal.h"

#include <image.h> /* Haiku: get_next_image_info → own executable path */

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

/* ---- Host-wide state (main thread unless noted) ---- */

static double ph_sr = 48000.0;
static int ph_maxblock = 1024;
static GList *ph_cat = NULL; /* PluginInfo* */
static gboolean ph_scanned = FALSE;

/* ---- Diagnostics (JACKDAW_DIAG) ---- */

static __thread int ph_rt_in_callback = 0;
extern "C" void ph_rt_mark(int on)
{
    ph_rt_in_callback = on;
}
extern "C" int ph_rt_active(void)
{
    return ph_rt_in_callback;
}

/* Count IHostApplication::createInstance calls made while the JACK RT thread
 * is inside the process callback: each is a heap allocation (new HostMessage /
 * HostAttributeList) on the RT thread — the classic VST3 metering-message
 * path that breaks the no-RT-malloc rule. RT thread is the single writer. */
static volatile guint64 g_vst3_rt_alloc_calls = 0;
extern "C" guint64 ph_vst3_rt_alloc_count(void)
{
    return g_vst3_rt_alloc_calls;
}

namespace
{

class CountingHostApp : public HostApplication
{
public:
    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void **obj) override
    {
        if (ph_rt_active())
            g_vst3_rt_alloc_calls++;
        return HostApplication::createInstance(cid, _iid, obj);
    }
};

} // namespace

/* Expose an IHostApplication to plug-ins via the plugin context, registered
 * once before any plug-in code runs. Plug-ins query it during initialize()
 * to create IMessage/IAttributeList objects (NAMku's file paths travel that
 * way); PlugProvider passes it to component+controller initialize(). */
static void ph_ensure_context(void)
{
    static bool done = false;
    if (!done) {
        PluginContextFactory::instance().setPluginContext(new CountingHostApp());
        done = true;
    }
}

/* ---- Backend ---- */

struct Vst3Backend {
    VST3::Hosting::Module::Ptr module;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    IPtr<IAudioProcessor> processor;
    IPtr<NAMku::INamFileLoader> file_loader; /* NULL unless the plug-in has one */
    HostProcessData data;
    ProcessContext ctx;
    ParameterChanges in_params;       /* RT-side, pre-allocated */
    ParameterChangeTransfer transfer; /* UI → RT lock-free ring */
    EventList in_events{256};         /* MIDI for instruments, pre-allocated */
    int max_block = 0;
    std::vector<ParamID> param_ids;
};

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
    Vst3Backend *b;

    /* Dry-signal scratch for the wet/dry mix (allocated to max_block). */
    float *dry_L, *dry_R;

    /* Worst-case µs in process() this period (RT writes, diag reader resets). */
    volatile gint64 diag_max_us;
};

/* ---- Small helpers ---- */

/* Path validation before loading plugin code: absolute, no "..", sane length. */
static gboolean ph_path_is_safe(const char *path)
{
    if (!path || path[0] != '/')
        return FALSE;
    size_t len = strlen(path);
    if (len < 6 || len > 4000) /* shortest: "/x.vst3" region */
        return FALSE;
    if (strstr(path, ".."))
        return FALSE;
    return TRUE;
}

static std::string ph_self_path(void)
{
    image_info info;
    int32 cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
        if (info.type == B_APP_IMAGE)
            return info.name;
    return {};
}

static PluginInfo *ph_info_new(const char *key, const char *name, const char *category)
{
    PluginInfo *pi = g_new0(PluginInfo, 1);
    pi->format = PH_VST3;
    pi->key = g_strdup(key);
    pi->name = g_strdup(name);
    pi->category = g_strdup(category);
    /* VST3 subcategory strings mark synths as "Instrument|..." */
    pi->is_instrument = category && strstr(category, "Instrument") != NULL;
    return pi;
}

/* ---- Transport (published by the RT thread, read by process_midi) ---- */

static double ph_xport_bpm = 120.0;
static double ph_xport_sr = 48000.0;
static gint64 ph_xport_frame = 0;
static gboolean ph_xport_playing = FALSE;

extern "C" void pluginhost_set_transport(double bpm, double sr, gint64 frame, gboolean playing)
{
    ph_xport_bpm = bpm;
    ph_xport_sr = sr;
    ph_xport_frame = frame;
    ph_xport_playing = playing;
}

static void ph_info_free(gpointer p)
{
    PluginInfo *pi = (PluginInfo *)p;
    g_free(pi->key);
    g_free(pi->name);
    g_free(pi->category);
    g_free(pi);
}

/* ---- Scan ----
 * The main process only ENUMERATES .vst3 bundles; each is described by a
 * throwaway `JackDAW --scan-plugin VST3 <path>` helper process, so a module
 * with a crashing static initializer kills the helper, not the DAW. The
 * helper prints one tab-separated "VST3\t<class name>\t<category>" line per
 * audio-effect class. (No on-disk scan cache yet: the catalog is built
 * lazily on first use and bundle counts on Haiku are small.) */

extern "C" int pluginhost_scan_helper_main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "--scan-plugin") != 0 || strcmp(argv[2], "VST3") != 0)
        return -1; /* not a scan invocation: continue normal startup */

    const char *path = argv[3];
    if (!ph_path_is_safe(path))
        return 1;

    ph_ensure_context();
    std::string err;
    auto mod = VST3::Hosting::Module::create(path, err);
    if (!mod) {
        fprintf(stderr, "scan: could not load '%s': %s\n", path, err.c_str());
        return 1;
    }
    auto factory = mod->getFactory();
    for (auto &ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass)
            continue;
        std::string name = ci.name();
        std::string subcat = ci.subCategoriesString();
        printf("VST3\t%s\t%s\n", name.c_str(), subcat.empty() ? "VST3" : subcat.c_str());
    }
    return 0;
}

/* Describe one bundle via the helper process and append its classes. */
static void ph_scan_bundle(const char *path, GList **catalog)
{
    std::string self = ph_self_path();
    if (self.empty())
        return;

    gchar *argv[] = {(gchar *)self.c_str(), (gchar *)"--scan-plugin", (gchar *)"VST3",
                     (gchar *)path, NULL};
    gchar *out = NULL;
    gint status = 0;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status,
                      NULL))
        return;
    if (out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            gchar **f = g_strsplit(lines[i], "\t", 3);
            if (f[0] && strcmp(f[0], "VST3") == 0 && f[1] && f[2]) {
                gchar *key = g_strdup_printf("%s\n%s", path, f[1]);
                *catalog = g_list_prepend(*catalog, ph_info_new(key, f[1], f[2]));
                g_free(key);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(out);
    }
}

static void ph_do_scan(void)
{
    /* Module::getModulePaths() (module_haiku.cpp — the same enumeration the
     * validator's -list uses) walks the Haiku VST3 locations (the add-on
     * directories from find_paths() plus $HOME/.vst3) and returns the .vst3
     * BUNDLE paths themselves, ready to describe. */
    for (auto &module_path : VST3::Hosting::Module::getModulePaths())
        ph_scan_bundle(module_path.c_str(), &ph_cat);
    ph_cat = g_list_reverse(ph_cat);
    ph_scanned = TRUE;
}

extern "C" const GList *pluginhost_catalog(void)
{
    if (!ph_scanned)
        ph_do_scan();
    return ph_cat;
}

extern "C" void pluginhost_rescan(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;
    ph_scanned = FALSE;
}

/* ---- Init / shutdown ---- */

extern "C" void pluginhost_init(double sample_rate, int max_block)
{
    ph_sr = sample_rate;
    ph_maxblock = MAX(max_block, 64);
    ph_ensure_context();
}

extern "C" void pluginhost_shutdown(void)
{
    g_list_free_full(ph_cat, ph_info_free);
    ph_cat = NULL;
    ph_scanned = FALSE;
}

/* ---- Instantiate / free ---- */

extern "C" PluginInstance *pluginhost_instantiate(const PluginInfo *info)
{
    if (!info || info->format != PH_VST3 || !info->key)
        return NULL;
    ph_ensure_context();

    gchar **parts = g_strsplit(info->key, "\n", 2);
    if (!parts[0] || !parts[1]) {
        g_strfreev(parts);
        return NULL;
    }
    std::string path = parts[0];
    std::string want = parts[1];
    g_strfreev(parts);
    if (!ph_path_is_safe(path.c_str()))
        return NULL;

    std::string err;
    auto module = VST3::Hosting::Module::create(path, err);
    if (!module) {
        g_warning("pluginhost: cannot load '%s': %s", path.c_str(), err.c_str());
        return NULL;
    }
    auto factory = module->getFactory();

    auto infos = factory.classInfos();
    const VST3::Hosting::ClassInfo *found = nullptr;
    for (auto &ci : infos) {
        if (ci.category() == kVstAudioEffectClass && ci.name() == want) {
            found = &ci;
            break;
        }
    }
    if (!found)
        return NULL;

    Vst3Backend *b = new Vst3Backend();
    b->module = module;
    b->max_block = ph_maxblock;
    b->provider = owned(new PlugProvider(factory, *found, true));
    if (!b->provider->initialize()) {
        delete b;
        return NULL;
    }
    b->component = b->provider->getComponentPtr();
    b->controller = b->provider->getControllerPtr();
    if (!b->component) {
        delete b;
        return NULL;
    }
    b->processor = FUnknownPtr<IAudioProcessor>(b->component);
    if (!b->processor) {
        delete b;
        return NULL;
    }

    /* Stereo in/out arrangement (best effort; a refusing plug-in keeps its
     * default and our buffers follow its actual bus channel counts). */
    SpeakerArrangement in = SpeakerArr::kStereo, out = SpeakerArr::kStereo;
    b->processor->setBusArrangements(&in, 1, &out, 1);

    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = ph_maxblock;
    setup.sampleRate = ph_sr;
    if (b->processor->setupProcessing(setup) != kResultOk)
        g_warning("pluginhost: '%s' refused setupProcessing", want.c_str());

    /* VST3 buses are INACTIVE by default ("The plug-in should only process an
     * activated bus") — without this most plug-ins emit silence. Activate
     * every default-active audio bus (the plug-in's main I/O). */
    for (int d = 0; d < 2; d++) {
        BusDirection bd = (d == 0) ? kInput : kOutput;
        int32 nb = b->component->getBusCount(kAudio, bd);
        for (int32 i = 0; i < nb; i++) {
            BusInfo bi;
            if (b->component->getBusInfo(kAudio, bd, i, bi) == kResultOk &&
                (bi.flags & BusInfo::kDefaultActive))
                b->component->activateBus(kAudio, bd, i, true);
        }
    }
    /* Instruments receive notes on an event INPUT bus, also inactive by
     * default. Activate every default-active one so MIDI reaches the synth. */
    {
        int32 nb = b->component->getBusCount(kEvent, kInput);
        for (int32 i = 0; i < nb; i++) {
            BusInfo bi;
            if (b->component->getBusInfo(kEvent, kInput, i, bi) == kResultOk &&
                (bi.flags & BusInfo::kDefaultActive))
                b->component->activateBus(kEvent, kInput, i, true);
        }
    }

    /* Buffer management by HostProcessData (prepared before activation). */
    if (!b->data.prepare(*b->component, ph_maxblock, kSample32)) {
        delete b;
        return NULL;
    }
    memset(&b->ctx, 0, sizeof(b->ctx));
    b->ctx.sampleRate = ph_sr;
    b->data.processContext = &b->ctx;
    b->data.processMode = kRealtime;
    b->data.symbolicSampleSize = kSample32;
    b->data.inputParameterChanges = &b->in_params;

    if (b->component->setActive(true) != kResultOk) {
        b->data.unprepare();
        delete b;
        return NULL;
    }
    b->processor->setProcessing(true);

    if (b->controller) {
        int32 pc = b->controller->getParameterCount();
        for (int32 i = 0; i < pc; i++) {
            ParameterInfo pinf;
            if (b->controller->getParameterInfo(i, pinf) == kResultOk)
                b->param_ids.push_back(pinf.id);
        }
        /* Pre-allocate both sides of the UI → RT path so the RT thread never
         * grows a queue: the ring, and the ParameterChanges it drains into. */
        b->transfer.setMaxParameters(MAX(256, (int32)b->param_ids.size() * 4));
        b->in_params.setMaxParameters((int32)b->param_ids.size());

        /* Optional file-loading extension (NAMku): discovered purely via
         * queryInterface, so unknown plug-ins are unaffected. */
        NAMku::INamFileLoader *fl = nullptr;
        if (b->controller->queryInterface(NAMku::INamFileLoader_iid, (void **)&fl) == kResultOk &&
            fl)
            b->file_loader = owned(fl);
    }

    PluginInstance *pi = g_new0(PluginInstance, 1);
    pi->format = PH_VST3;
    pi->name = g_strdup(info->name);
    pi->key = g_strdup(info->key);
    pi->category = g_strdup(info->category);
    pi->is_instrument =
        info->is_instrument || (info->category && strstr(info->category, "Instrument"));
    pi->active = 1;
    pi->mix_q15 = 32768;
    pi->sample_rate = ph_sr;
    pi->max_block = ph_maxblock;
    pi->dry_L = g_new0(float, ph_maxblock);
    pi->dry_R = g_new0(float, ph_maxblock);
    pi->b = b;
    return pi;
}

extern "C" void pluginhost_free(PluginInstance *inst)
{
    if (!inst)
        return;
    Vst3Backend *b = inst->b;
    if (b) {
        b->file_loader = nullptr; /* before the controller goes away */
        if (b->processor)
            b->processor->setProcessing(false);
        if (b->component)
            b->component->setActive(false);
        b->data.unprepare();
        b->processor = nullptr;
        b->controller = nullptr;
        b->component = nullptr;
        b->provider = nullptr;
        b->module = nullptr;
        delete b;
    }
    g_free(inst->dry_L);
    g_free(inst->dry_R);
    g_free(inst->name);
    g_free(inst->key);
    g_free(inst->category);
    g_free(inst);
}

/* ---- RT processing ---- */

static gboolean ph_diag_enabled(void)
{
    static int e = -1;
    if (e < 0)
        e = (g_getenv("JACKDAW_DIAG") != NULL) ? 1 : 0;
    return e;
}

static inline gint64 ph_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

extern "C" gint64 pluginhost_diag_take_max_us(PluginInstance *inst)
{
    if (!inst)
        return 0;
    gint64 v = inst->diag_max_us;
    inst->diag_max_us = 0;
    return v;
}

extern "C" void pluginhost_process(PluginInstance *inst, float *L, float *R, int nframes)
{
    if (!inst || !inst->b || !inst->b->processor)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave L/R as-is */
    Vst3Backend *b = inst->b;
    if (nframes > b->max_block)
        return;

    int mq = g_atomic_int_get(&inst->mix_q15);
    gboolean blend = (mq < 32768) && inst->dry_L && inst->dry_R;
    if (blend) { /* stash the dry signal for the mix */
        memcpy(inst->dry_L, L, sizeof(float) * (size_t)nframes);
        memcpy(inst->dry_R, R, sizeof(float) * (size_t)nframes);
    }

    /* Re-arm FTZ/DAZ: the previous plugin (or this one) may have cleared
     * MXCSR, which would let denormals stall this plugin's process(). */
    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled() ? ph_now_us() : 0;

    b->data.numSamples = nframes;
    if (b->data.inputs && b->data.inputs[0].channelBuffers32) {
        memcpy(b->data.inputs[0].channelBuffers32[0], L, sizeof(float) * (size_t)nframes);
        if (b->data.inputs[0].numChannels > 1)
            memcpy(b->data.inputs[0].channelBuffers32[1], R, sizeof(float) * (size_t)nframes);
    }
    /* Zero the outputs first: if the plugin fails to process we get silence
     * rather than stale garbage. */
    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        for (int ch = 0; ch < b->data.outputs[0].numChannels; ch++)
            memset(b->data.outputs[0].channelBuffers32[ch], 0, sizeof(float) * (size_t)nframes);
    }

    /* Drain the UI-side ring into the pre-allocated RT ParameterChanges. */
    b->transfer.transferChangesTo(b->in_params);
    b->processor->process(b->data);
    b->in_params.clearQueue();

    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        memcpy(L, b->data.outputs[0].channelBuffers32[0], sizeof(float) * (size_t)nframes);
        int oc = b->data.outputs[0].numChannels;
        memcpy(R, b->data.outputs[0].channelBuffers32[oc > 1 ? 1 : 0],
               sizeof(float) * (size_t)nframes);
    }

    if (t0) {
        gint64 d = ph_now_us() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    /* Safety net: a misbehaving plugin must never send NaN/inf or a runaway
     * level to the speakers. Replace non-finite samples, clamp magnitude. */
    for (int i = 0; i < nframes; i++) {
        float a = L[i], c = R[i];
        if (!std::isfinite(a))
            a = 0.0f;
        else if (a > 4.0f)
            a = 4.0f;
        else if (a < -4.0f)
            a = -4.0f;
        if (!std::isfinite(c))
            c = 0.0f;
        else if (c > 4.0f)
            c = 4.0f;
        else if (c < -4.0f)
            c = -4.0f;
        L[i] = a;
        R[i] = c;
    }

    if (blend) { /* wet/dry crossfade */
        float wet = mq * (1.0f / 32768.0f);
        float dry = 1.0f - wet;
        for (int i = 0; i < nframes; i++) {
            L[i] = inst->dry_L[i] * dry + L[i] * wet;
            R[i] = inst->dry_R[i] * dry + R[i] * wet;
        }
    }
}

extern "C" gboolean pluginhost_is_instrument(PluginInstance *inst)
{
    return inst ? inst->is_instrument : FALSE;
}

extern "C" void pluginhost_process_midi(PluginInstance *inst, const PhMidiEvent *ev, int n_ev,
                                        float *L, float *R, int nframes)
{
    if (!inst || !inst->b || !inst->b->processor)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave the caller's (silent) buffers as-is */
    Vst3Backend *b = inst->b;
    if (nframes > b->max_block)
        return;

    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled() ? ph_now_us() : 0;

    /* This block's MIDI as VST3 note events (pre-allocated event list). */
    b->in_events.clear();
    for (int i = 0; i < n_ev; i++) {
        const guint8 *m = ev[i].data;
        uint8 status = m[0] & 0xF0, ch = m[0] & 0x0F;
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = (int32)ev[i].time;
        e.flags = Event::kIsLive;
        if (status == 0x90 && m[2] > 0) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = ch;
            e.noteOn.pitch = m[1];
            e.noteOn.velocity = m[2] / 127.0f;
            e.noteOn.noteId = -1;
        } else if (status == 0x80 || (status == 0x90 && m[2] == 0)) {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = ch;
            e.noteOff.pitch = m[1];
            e.noteOff.velocity = (status == 0x80) ? m[2] / 127.0f : 0.0f;
            e.noteOff.noteId = -1;
        } else {
            continue; /* CC/other: not delivered yet (matches the Linux host) */
        }
        b->in_events.addEvent(e);
    }
    b->data.inputEvents = &b->in_events;

    /* Transport/tempo for the plug-in's ProcessContext (arps, tempo-synced
     * LFOs). Published by the engine at the top of this same RT cycle. */
    b->ctx.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
    if (ph_xport_playing)
        b->ctx.state |= ProcessContext::kPlaying;
    b->ctx.sampleRate = ph_xport_sr;
    b->ctx.tempo = ph_xport_bpm;
    b->ctx.projectTimeSamples = (TSamples)ph_xport_frame;
    double fpb = (ph_xport_bpm > 0.0) ? ph_xport_sr * 60.0 / ph_xport_bpm : 0.0;
    b->ctx.projectTimeMusic = (fpb > 0.0) ? (double)ph_xport_frame / fpb : 0.0;

    b->data.numSamples = nframes;
    if (b->data.inputs && b->data.inputs[0].channelBuffers32) /* silence in */
        for (int ch = 0; ch < b->data.inputs[0].numChannels; ch++)
            memset(b->data.inputs[0].channelBuffers32[ch], 0, sizeof(float) * (size_t)nframes);
    if (b->data.outputs && b->data.outputs[0].channelBuffers32)
        for (int ch = 0; ch < b->data.outputs[0].numChannels; ch++)
            memset(b->data.outputs[0].channelBuffers32[ch], 0, sizeof(float) * (size_t)nframes);

    b->transfer.transferChangesTo(b->in_params);
    b->processor->process(b->data);
    b->in_params.clearQueue();
    b->in_events.clear();
    b->data.inputEvents = nullptr; /* reset for the effect (audio) path */

    if (b->data.outputs && b->data.outputs[0].channelBuffers32) {
        memcpy(L, b->data.outputs[0].channelBuffers32[0], sizeof(float) * (size_t)nframes);
        int oc = b->data.outputs[0].numChannels;
        memcpy(R, b->data.outputs[0].channelBuffers32[oc > 1 ? 1 : 0],
               sizeof(float) * (size_t)nframes);
    }

    if (t0) {
        gint64 d = ph_now_us() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    /* Same speaker-safety net as pluginhost_process. */
    for (int i = 0; i < nframes; i++) {
        float a = L[i], c = R[i];
        if (!std::isfinite(a))
            a = 0.0f;
        else if (a > 4.0f)
            a = 4.0f;
        else if (a < -4.0f)
            a = -4.0f;
        if (!std::isfinite(c))
            c = 0.0f;
        else if (c > 4.0f)
            c = 4.0f;
        else if (c < -4.0f)
            c = -4.0f;
        L[i] = a;
        R[i] = c;
    }
}

/* ---- Reset / state ---- */

extern "C" void pluginhost_reset(PluginInstance *inst)
{
    if (!inst || !inst->b)
        return;
    Vst3Backend *b = inst->b;
    /* Inactive→active cycle resets the processor's internal state. */
    if (b->processor)
        b->processor->setProcessing(false);
    if (b->component) {
        b->component->setActive(false);
        b->component->setActive(true);
    }
    if (b->processor)
        b->processor->setProcessing(true);
}

/* Container: two little-endian length-prefixed blobs —
 * [int32 comp_len][comp bytes][int32 ctrl_len][ctrl bytes]. The component
 * stream is what the parameter list cannot capture (NAMku's .nam/.wav paths,
 * internal modes); the controller stream is controller-private extras. */
extern "C" gboolean pluginhost_state_save(PluginInstance *inst, void **out, gsize *out_len)
{
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!inst || !inst->b || !inst->b->component || !out || !out_len)
        return FALSE;
    Vst3Backend *b = inst->b;

    MemoryStream comp, ctrl;
    if (b->component->getState(&comp) != kResultOk)
        return FALSE;
    gboolean have_ctrl = (b->controller && b->controller->getState(&ctrl) == kResultOk);

    gint32 clen = (gint32)comp.getSize();
    gint32 elen = have_ctrl ? (gint32)ctrl.getSize() : 0;
    if (clen < 0 || elen < 0)
        return FALSE;

    gsize total = 8 + (gsize)clen + (gsize)elen;
    guint8 *buf = (guint8 *)g_malloc(total);
    gint32 cle = GINT32_TO_LE(clen), ele = GINT32_TO_LE(elen);
    memcpy(buf + 0, &cle, 4);
    memcpy(buf + 4, &ele, 4);
    if (clen > 0)
        memcpy(buf + 8, comp.getData(), (size_t)clen);
    if (elen > 0)
        memcpy(buf + 8 + clen, ctrl.getData(), (size_t)elen);

    *out = buf;
    *out_len = total;
    return TRUE;
}

extern "C" gboolean pluginhost_state_load(PluginInstance *inst, const void *data, gsize len)
{
    if (!inst || !inst->b || !inst->b->component || !data || len < 8)
        return FALSE;
    Vst3Backend *b = inst->b;

    /* The blob is untrusted project-file input: validate the lengths. */
    const guint8 *p = (const guint8 *)data;
    gint32 clen, elen;
    memcpy(&clen, p + 0, 4);
    clen = GINT32_FROM_LE(clen);
    memcpy(&elen, p + 4, 4);
    elen = GINT32_FROM_LE(elen);
    if (clen < 0 || elen < 0 || (gsize)8 + (gsize)clen + (gsize)elen > len)
        return FALSE;

    /* Restore the processor (component) state, then hand the SAME stream to
     * the controller via setComponentState so its parameter view matches. */
    if (clen > 0) {
        MemoryStream cs((void *)(p + 8), clen);
        b->component->setState(&cs);
        cs.seek(0, IBStream::kIBSeekSet, nullptr);
        if (b->controller)
            b->controller->setComponentState(&cs);
    }
    if (elen > 0 && b->controller) {
        MemoryStream es((void *)(p + 8 + clen), elen);
        b->controller->setState(&es);
    }

    /* Mirror the restored normalized values through the UI → RT ring so the
     * live processor matches the controller on the very next block. */
    if (b->controller) {
        for (size_t i = 0; i < b->param_ids.size(); i++) {
            ParamValue v = b->controller->getParamNormalized(b->param_ids[i]);
            b->transfer.addChange(b->param_ids[i], v, 0);
        }
    }
    return TRUE;
}

/* ---- Flags / identity ---- */

extern "C" void pluginhost_set_active(PluginInstance *inst, gboolean on)
{
    if (inst)
        g_atomic_int_set(&inst->active, on ? 1 : 0);
}

extern "C" gboolean pluginhost_is_active(PluginInstance *inst)
{
    return inst ? g_atomic_int_get(&inst->active) != 0 : FALSE;
}

extern "C" void pluginhost_set_mix(PluginInstance *inst, float mix)
{
    if (!inst)
        return;
    if (mix < 0.0f)
        mix = 0.0f;
    if (mix > 1.0f)
        mix = 1.0f;
    g_atomic_int_set(&inst->mix_q15, (gint)lrintf(mix * 32768.0f));
}

extern "C" float pluginhost_get_mix(PluginInstance *inst)
{
    return inst ? g_atomic_int_get(&inst->mix_q15) * (1.0f / 32768.0f) : 1.0f;
}

extern "C" const char *pluginhost_name(PluginInstance *inst)
{
    return inst ? inst->name : "";
}

extern "C" PluginFormat pluginhost_format(PluginInstance *inst)
{
    return inst ? inst->format : PH_VST3;
}

extern "C" const char *pluginhost_key(PluginInstance *inst)
{
    return inst ? inst->key : "";
}

extern "C" const char *pluginhost_category(PluginInstance *inst)
{
    return inst ? inst->category : "";
}

/* ---- Parameters (owning window thread only) ---- */

extern "C" guint pluginhost_param_count(PluginInstance *inst)
{
    return (inst && inst->b && inst->b->controller) ? (guint)inst->b->param_ids.size() : 0;
}

extern "C" void pluginhost_param_name(PluginInstance *inst, guint i, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return;
    buf[0] = 0;
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return;
    ParameterInfo info;
    if (inst->b->controller->getParameterInfo((int32)i, info) == kResultOk)
        g_strlcpy(buf, StringConvert::convert(info.title).c_str(), (gsize)buflen);
}

extern "C" float pluginhost_param_get(PluginInstance *inst, guint i)
{
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return 0.0f;
    return (float)inst->b->controller->getParamNormalized(inst->b->param_ids[i]);
}

extern "C" void pluginhost_param_set(PluginInstance *inst, guint i, float v)
{
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    Vst3Backend *b = inst->b;
    /* Keep the controller's view in sync, then hand the change to the RT
     * thread through the lock-free ring (drained in pluginhost_process). */
    b->controller->setParamNormalized(b->param_ids[i], v);
    b->transfer.addChange(b->param_ids[i], v, 0);
}

extern "C" void pluginhost_param_display(PluginInstance *inst, guint i, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return;
    buf[0] = 0;
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return;
    Vst3Backend *b = inst->b;
    ParameterInfo info;
    if (b->controller->getParameterInfo((int32)i, info) != kResultOk)
        return;
    ParamValue norm = b->controller->getParamNormalized(info.id);
    String128 display{};
    std::string text;
    if (b->controller->getParamStringByValue(info.id, norm, display) == kResultOk)
        text = StringConvert::convert(display);
    std::string units = StringConvert::convert(info.units);
    if (!units.empty())
        text += " " + units;
    g_strlcpy(buf, text.c_str(), (gsize)buflen);
}

extern "C" gboolean pluginhost_param_is_stepped(PluginInstance *inst, guint i, gint *steps)
{
    if (steps)
        *steps = 1;
    if (!inst || !inst->b || !inst->b->controller || i >= inst->b->param_ids.size())
        return FALSE;
    ParameterInfo info;
    if (inst->b->controller->getParameterInfo((int32)i, info) != kResultOk)
        return FALSE;
    if (info.stepCount <= 0)
        return FALSE;
    if (steps)
        *steps = info.stepCount;
    return TRUE;
}

/* ---- File loading (INamFileLoader) ---- */

extern "C" gboolean pluginhost_has_file_loader(PluginInstance *inst)
{
    return inst && inst->b && inst->b->file_loader;
}

extern "C" gboolean pluginhost_file_get(PluginInstance *inst, int which, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return FALSE;
    buf[0] = 0;
    if (!pluginhost_has_file_loader(inst))
        return FALSE;
    NAMku::INamFileLoader *fl = inst->b->file_loader;
    tresult r =
        (which == PH_FILE_MODEL) ? fl->getModelFile(buf, buflen) : fl->getIrFile(buf, buflen);
    return r == kResultOk;
}

extern "C" gboolean pluginhost_file_set(PluginInstance *inst, int which, const char *path)
{
    if (!pluginhost_has_file_loader(inst))
        return FALSE;
    NAMku::INamFileLoader *fl = inst->b->file_loader;
    tresult r = (which == PH_FILE_MODEL) ? fl->setModelFile(path ? path : "")
                                         : fl->setIrFile(path ? path : "");
    return r == kResultOk;
}
