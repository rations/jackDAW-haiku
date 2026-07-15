/* pluginhost_lv2.cpp — LV2 backend for the jackDAW plugin host.
 *
 * Built on lilv/zix (the LV2-haiku sibling project's native stack). The
 * hosting core — feature allow-list, dual-mono instantiation, worker
 * extension, lock-free control ring — is adapted from that project's shared
 * host core, reshaped to run inside the DAW engine's JACK process callback
 * instead of owning a JACK client of its own.
 *
 * Threading: scan/instantiate/free/params/state/UI run on the instance's
 * owning window looper; ph_lv2_process() runs on the JACK RT thread and
 * never allocates, locks, or logs. UI → RT control changes go through a
 * pre-allocated lock-free zix ring drained at the top of the RT call. The
 * non-RT side keeps its own shadow copy of input control values, so reads
 * never race the RT store (output/meter ports are read from the RT store as
 * single aligned-float loads). One process-wide lilv world (guarded by a
 * non-RT mutex) outlives every instance; it is freed only at shutdown.
 *
 * Native editors: HaikuUI (LV2UI_Widget is a detached BView*, the ui:CocoaUI
 * pattern). This TU never dereferences the view — it hands it to the FX
 * window as void*, delivers port_event() ticks, and the UI's own cleanup()
 * detaches and deletes the view.
 */

#include "pluginhost.h"
#include "pluginhost_internal.h"

#include "engine/rt_denormal.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <zix/ring.h>
#include <zix/sem.h>
#include <zix/thread.h>

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The HaikuUI LV2 UI class (LV2UI_Widget == detached BView*), defined by the
 * ui-haiku extension bundle; the URI is the specification. */
#define LPH_HAIKU_UI_URI "https://github.com/rations/LV2-haiku/ns/ui-haiku#HaikuUI"

#define LPH_NAME_MAX 96
#define LPH_SYM_MAX 96
#define LPH_RING_BYTES 8192
#define LPH_MAX_PACKET 4096
#define LPH_STATE_MAX (16u * 1024u * 1024u) /* untrusted .jdaw input bound */

/* ---- Host-wide state (owning window threads; never the RT thread) ---- */

static double lph_sr = 48000.0;
static int lph_maxblock = 1024;

/* One lilv world for the whole process. lilv is not thread-safe and non-RT
 * calls arrive from more than one window looper (FX windows, project load on
 * the main window), so every world-touching section takes this mutex. The RT
 * path never does. */
static GMutex lph_world_lock;
static LilvWorld *lph_world = NULL;

/* Borrowed pointers into the world, valid while the world lives. */
static LilvNode *lph_n_audio, *lph_n_control, *lph_n_input, *lph_n_output;
static LilvNode *lph_n_atom, *lph_n_cv, *lph_n_toggled, *lph_n_integer, *lph_n_enum;

static LilvWorld *lph_world_get_locked(void)
{
    if (!lph_world) {
        lph_world = lilv_world_new();
        if (!lph_world)
            return NULL;
        lilv_world_load_all(lph_world);
        lph_n_audio = lilv_new_uri(lph_world, LILV_URI_AUDIO_PORT);
        lph_n_control = lilv_new_uri(lph_world, LILV_URI_CONTROL_PORT);
        lph_n_input = lilv_new_uri(lph_world, LILV_URI_INPUT_PORT);
        lph_n_output = lilv_new_uri(lph_world, LILV_URI_OUTPUT_PORT);
        lph_n_atom = lilv_new_uri(lph_world, LV2_ATOM__AtomPort);
        lph_n_cv = lilv_new_uri(lph_world, LV2_CORE__CVPort);
        lph_n_toggled = lilv_new_uri(lph_world, LV2_CORE__toggled);
        lph_n_integer = lilv_new_uri(lph_world, LV2_CORE__integer);
        lph_n_enum = lilv_new_uri(lph_world, LV2_CORE__enumeration);
    }
    return lph_world;
}

extern "C" void ph_lv2_init(double sample_rate, int max_block)
{
    lph_sr = sample_rate;
    lph_maxblock = MAX(max_block, 64);
}

extern "C" void ph_lv2_shutdown(void)
{
    g_mutex_lock(&lph_world_lock);
    if (lph_world) {
        lilv_node_free(lph_n_audio);
        lilv_node_free(lph_n_control);
        lilv_node_free(lph_n_input);
        lilv_node_free(lph_n_output);
        lilv_node_free(lph_n_atom);
        lilv_node_free(lph_n_cv);
        lilv_node_free(lph_n_toggled);
        lilv_node_free(lph_n_integer);
        lilv_node_free(lph_n_enum);
        lilv_world_free(lph_world);
        lph_world = NULL;
    }
    g_mutex_unlock(&lph_world_lock);
}

/* ---- URID map/unmap (process-wide; growth only on the non-RT map path) ---- */

typedef struct {
    char **uris; /* index = id - 1 */
    uint32_t n;
    uint32_t cap;
    pthread_mutex_t lock;
} LphUridTable;

static LphUridTable lph_urids = {NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER};

static LV2_URID lph_urid_map_cb(LV2_URID_Map_Handle handle, const char *uri)
{
    (void)handle;
    if (!uri || !*uri)
        return 0;
    pthread_mutex_lock(&lph_urids.lock);
    for (uint32_t i = 0; i < lph_urids.n; i++) {
        if (!strcmp(lph_urids.uris[i], uri)) {
            pthread_mutex_unlock(&lph_urids.lock);
            return i + 1;
        }
    }
    if (lph_urids.n == lph_urids.cap) {
        uint32_t cap = lph_urids.cap ? lph_urids.cap * 2 : 64;
        char **grown = (char **)realloc(lph_urids.uris, cap * sizeof(char *));
        if (!grown) {
            pthread_mutex_unlock(&lph_urids.lock);
            return 0;
        }
        lph_urids.uris = grown;
        lph_urids.cap = cap;
    }
    char *dup = strdup(uri);
    if (!dup) {
        pthread_mutex_unlock(&lph_urids.lock);
        return 0;
    }
    lph_urids.uris[lph_urids.n++] = dup;
    LV2_URID id = lph_urids.n;
    pthread_mutex_unlock(&lph_urids.lock);
    return id;
}

static const char *lph_urid_unmap_cb(LV2_URID_Unmap_Handle handle, LV2_URID urid)
{
    (void)handle;
    const char *s = NULL;
    pthread_mutex_lock(&lph_urids.lock);
    if (urid >= 1 && urid <= lph_urids.n)
        s = lph_urids.uris[urid - 1];
    pthread_mutex_unlock(&lph_urids.lock);
    return s;
}

static LV2_URID_Map lph_urid_map = {NULL, lph_urid_map_cb};
static LV2_URID_Unmap lph_urid_unmap = {NULL, lph_urid_unmap_cb};
static LV2_Feature lph_feat_map = {LV2_URID__map, &lph_urid_map};
static LV2_Feature lph_feat_unmap = {LV2_URID__unmap, &lph_urid_unmap};
static LV2_Feature lph_feat_bounded = {LV2_BUF_SIZE__boundedBlockLength, NULL};
static LV2_Feature lph_feat_powof2 = {LV2_BUF_SIZE__powerOf2BlockLength, NULL};

/* ---- Log feature (plugins may log from instantiate; never our RT path) ---- */

static int lph_log_vprintf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, va_list ap)
{
    (void)handle;
    (void)type;
    return vfprintf(stderr, fmt, ap);
}

static int lph_log_printf(LV2_Log_Handle handle, LV2_URID type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = lph_log_vprintf(handle, type, fmt, ap);
    va_end(ap);
    return r;
}

static LV2_Log_Log lph_log = {NULL, lph_log_printf, lph_log_vprintf};
static LV2_Feature lph_feat_log = {LV2_LOG__log, &lph_log};

/* ---- Worker extension (jalv model): the RT thread schedules jobs into a
 * lock-free ring; a dedicated non-RT thread per instance runs work();
 * responses return through a second ring and are applied from the RT context
 * after run(). ---- */

typedef struct {
    const LV2_Worker_Interface *iface;
    LV2_Handle handle;
    ZixRing *requests;
    ZixRing *responses;
    ZixSem sem;
    ZixThread thread;
    volatile int quit;
    bool active;
    LV2_Worker_Schedule schedule;
    LV2_Feature feature;
} LphWorker;

static LV2_Worker_Status lph_worker_write_packet(ZixRing *target, uint32_t size, const void *data)
{
    ZixRingTransaction tx = zix_ring_begin_write(target);
    if (zix_ring_amend_write(target, &tx, &size, sizeof(size)) ||
        zix_ring_amend_write(target, &tx, data, size))
        return LV2_WORKER_ERR_NO_SPACE;
    zix_ring_commit_write(target, &tx);
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status lph_worker_respond_cb(LV2_Worker_Respond_Handle handle, uint32_t size,
                                               const void *data)
{
    LphWorker *w = (LphWorker *)handle;
    return lph_worker_write_packet(w->responses, size, data);
}

static ZixThreadResult lph_worker_thread_fn(void *arg)
{
    LphWorker *w = (LphWorker *)arg;
    char buf[LPH_MAX_PACKET];
    for (;;) {
        zix_sem_wait(&w->sem);
        if (w->quit)
            break;
        uint32_t size = 0;
        while (zix_ring_read_space(w->requests) >= sizeof(size)) {
            zix_ring_read(w->requests, &size, sizeof(size));
            if (size > sizeof(buf)) { /* oversized: drain and drop */
                zix_ring_skip(w->requests, size);
                continue;
            }
            zix_ring_read(w->requests, buf, size);
            if (w->iface && w->iface->work)
                w->iface->work(w->handle, lph_worker_respond_cb, w, size, buf);
        }
    }
    return (ZixThreadResult)0;
}

/* RT-safe: copy the job into the ring and wake the worker. No malloc/lock. */
static LV2_Worker_Status lph_worker_schedule_cb(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                                const void *data)
{
    LphWorker *w = (LphWorker *)handle;
    if (!w->requests)
        return LV2_WORKER_ERR_UNKNOWN;
    LV2_Worker_Status st = lph_worker_write_packet(w->requests, size, data);
    if (st == LV2_WORKER_SUCCESS)
        zix_sem_post(&w->sem);
    return st;
}

/* Apply pending responses then end_run — called from the RT thread. */
static void lph_worker_apply_responses(LphWorker *w)
{
    if (!w->active || !w->iface)
        return;
    char buf[LPH_MAX_PACKET];
    uint32_t size = 0;
    while (zix_ring_read_space(w->responses) >= sizeof(size)) {
        zix_ring_read(w->responses, &size, sizeof(size));
        if (size > sizeof(buf)) {
            zix_ring_skip(w->responses, size);
            continue;
        }
        zix_ring_read(w->responses, buf, size);
        if (w->iface->work_response)
            w->iface->work_response(w->handle, size, buf);
    }
    if (w->iface->end_run)
        w->iface->end_run(w->handle);
}

static void lph_worker_init(LphWorker *w, LilvInstance *inst)
{
    const LV2_Worker_Interface *iface =
        (const LV2_Worker_Interface *)lilv_instance_get_extension_data(inst, LV2_WORKER__interface);
    if (!iface) {
        w->active = false;
        return;
    }
    w->iface = iface;
    w->handle = lilv_instance_get_handle(inst);
    w->requests = zix_ring_new(NULL, LPH_RING_BYTES);
    w->responses = zix_ring_new(NULL, LPH_RING_BYTES);
    if (!w->requests || !w->responses)
        goto fail;
    if (zix_ring_mlock(w->requests) != ZIX_STATUS_SUCCESS ||
        zix_ring_mlock(w->responses) != ZIX_STATUS_SUCCESS)
        g_warning("pluginhost_lv2: failed to lock worker rings in memory");
    if (zix_sem_init(&w->sem, 0) != ZIX_STATUS_SUCCESS)
        goto fail;
    w->quit = 0;
    if (zix_thread_create(&w->thread, 65536, lph_worker_thread_fn, w) != ZIX_STATUS_SUCCESS) {
        zix_sem_destroy(&w->sem);
        goto fail;
    }
    w->active = true;
    return;

fail:
    zix_ring_free(w->requests);
    zix_ring_free(w->responses);
    w->requests = NULL;
    w->responses = NULL;
    w->active = false;
}

static void lph_worker_destroy(LphWorker *w)
{
    if (!w->active)
        return;
    w->quit = 1;
    zix_sem_post(&w->sem);
    zix_thread_join(w->thread);
    zix_sem_destroy(&w->sem);
    zix_ring_free(w->requests);
    zix_ring_free(w->responses);
    w->active = false;
}

/* ---- Feature allow-list: a plugin that requires anything else is rejected
 * at scan time (never listed) rather than crashing at run time. ---- */

static bool lph_feature_supported(const char *uri)
{
    static const char *ok[] = {LV2_URID__map,
                               LV2_URID__unmap,
                               LV2_OPTIONS__options,
                               LV2_BUF_SIZE__boundedBlockLength,
                               LV2_BUF_SIZE__powerOf2BlockLength,
                               LV2_WORKER__schedule,
                               LV2_LOG__log,
                               NULL};
    for (int i = 0; ok[i]; i++)
        if (!strcmp(uri, ok[i]))
            return true;
    return false;
}

static bool lph_required_features_ok(const LilvPlugin *p)
{
    bool ok = true;
    LilvNodes *req = lilv_plugin_get_required_features(p);
    if (req) {
        LILV_FOREACH(nodes, it, req)
        {
            const LilvNode *f = lilv_nodes_get(req, it);
            const char *uri = lilv_node_as_uri(f);
            if (!uri || !lph_feature_supported(uri))
                ok = false;
        }
        lilv_nodes_free(req);
    }
    return ok;
}

/* ---- Backend ---- */

typedef struct {
    float value;
    char label[48];
} LphScalePoint;

typedef struct {
    uint32_t port_index;
    char name[LPH_NAME_MAX];
    char sym[LPH_SYM_MAX]; /* lv2:symbol — the state-file port identity */
    float min, max, def;
    bool toggled, integer, enumeration;
    LphScalePoint *points;
    uint32_t n_points;
} LphParam;

typedef struct {
    uint32_t index;
    bool is_input;
    uint32_t capacity;
    void *buf[2]; /* per instance (dual-mono) */
} LphAtomPort;

struct Lv2Backend {
    const LilvPlugin *plugin; /* borrowed from the process-wide world */
    LilvInstance *inst[2];
    int n_inst;
    bool dual_mono;

    uint32_t n_ports;
    float *ctl;    /* per-port RT value store (RT drains the ring into it;
                    * plugins write their output/meter ports into it) */
    float *shadow; /* owning-thread view of input control values */

    uint32_t *ain, *aout;
    int n_audio_in, n_audio_out;

    LphParam *params;
    uint32_t n_params;
    uint32_t *out_ctl; /* output control ports (meters/latency), for the UI */
    uint32_t n_out_ctl;

    LphAtomPort *atoms;
    uint32_t n_atoms;
    LV2_URID urid_seq, urid_chunk, urid_float;

    float *outA, *outB;          /* plugin output scratch (max_block) */
    float *dummy_in, *dummy_out; /* surplus audio / CV ports */
    void *misc;                  /* zeroed buffer for any leftover port type */
    uint32_t misc_bytes;

    int32_t opt_minblk, opt_maxblk, opt_seqsize;
    float opt_srate;
    LV2_Options_Option options[5];
    LV2_Feature feat_opts;
    LphWorker workers[2];
    const LV2_Feature *features[2][10];

    ZixRing *ctl_ring; /* control writes: owning/UI threads -> RT */

    /* Native editor (HaikuUI), resolved at instantiate, loaded on demand. */
    char *ui_uri;    /* NULL = plugin has no HaikuUI */
    char *ui_binary; /* filesystem path of the UI shared object */
    char *ui_bundle; /* filesystem path of the UI bundle directory */
    void *ui_dl;
    const LV2UI_Descriptor *ui_desc;
    LV2UI_Handle ui_handle;
    void *ui_widget; /* the BView*, opaque here */
    float *ui_last;  /* per-port last value delivered via port_event */
};

typedef struct {
    uint32_t port;
    float value;
} LphCtlWrite;

/* Find the parameter record for a port index (NULL if not an input control). */
static LphParam *lph_param_for_port(Lv2Backend *b, uint32_t port)
{
    for (uint32_t i = 0; i < b->n_params; i++)
        if (b->params[i].port_index == port)
            return &b->params[i];
    return NULL;
}

/* Enqueue a control write for the RT thread (clamped to the port's range).
 * Callable from any non-RT thread; the ring is single-reader (RT). */
static void lph_enqueue_ctl(Lv2Backend *b, uint32_t port, float val)
{
    if (!b->ctl_ring || port >= b->n_ports)
        return;
    LphCtlWrite cw;
    cw.port = port;
    cw.value = val;
    ZixRingTransaction tx = zix_ring_begin_write(b->ctl_ring);
    if (zix_ring_amend_write(b->ctl_ring, &tx, &cw, sizeof(cw)) == ZIX_STATUS_SUCCESS)
        zix_ring_commit_write(b->ctl_ring, &tx);
}

/* Owning-thread write: keep the shadow (UI truth) in sync and hand the value
 * to the RT thread. */
static void lph_write_control(PluginInstance *inst, uint32_t port, float val)
{
    Lv2Backend *b = inst->lv2;
    LphParam *pr = lph_param_for_port(b, port);
    if (pr) {
        if (val < pr->min)
            val = pr->min;
        if (val > pr->max)
            val = pr->max;
    }
    if (port < b->n_ports)
        b->shadow[port] = val;
    lph_enqueue_ctl(b, port, val);
}

/* ---- Scan ---- */

/* Skip plugins whose DSP binary is missing or is not a native Haiku shared
 * object (e.g. bundles copied verbatim from a Linux machine: broken .so
 * symlinks, glibc-linked builds the runtime loader rejects). dlopen'ing
 * plugin code during a scan is exactly what the scan design avoids, so this
 * is a static check: the library file must exist, be ELF, and reference
 * libroot.so in its dynamic strings — every native Haiku shared object does,
 * and no Linux build ever does. */
static gboolean lph_binary_is_loadable(const LilvPlugin *p)
{
    const LilvNode *lib = lilv_plugin_get_library_uri(p);
    if (!lib)
        return FALSE;
    char *path = lilv_file_uri_parse(lilv_node_as_uri(lib), NULL);
    if (!path)
        return FALSE;
    gboolean ok = FALSE;
    gchar *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(path, &data, &len, NULL)) {
        if (len > 4 && len <= 64u * 1024u * 1024u && memcmp(data, "\177ELF", 4) == 0) {
            /* Byte-wise search (the buffer is full of NULs, so no str* API). */
            static const char needle[] = "libroot.so";
            const gsize nlen = sizeof(needle) - 1;
            for (gsize i = 0; i + nlen <= len; i++) {
                if (data[i] == needle[0] && memcmp(data + i, needle, nlen) == 0) {
                    ok = TRUE;
                    break;
                }
            }
        }
        g_free(data);
    }
    lilv_free(path);
    return ok;
}

extern "C" void ph_lv2_scan(GList **catalog)
{
    g_mutex_lock(&lph_world_lock);
    LilvWorld *world = lph_world_get_locked();
    if (!world) {
        g_mutex_unlock(&lph_world_lock);
        return;
    }
    /* Pick up bundles installed since the last scan (additive: removed
     * bundles keep a stale entry until restart; instantiation then fails
     * with a warning instead of listing nothing). */
    lilv_world_load_all(world);

    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
    LILV_FOREACH(plugins, it, plugins)
    {
        const LilvPlugin *p = lilv_plugins_get(plugins, it);
        const char *uri = lilv_node_as_uri(lilv_plugin_get_uri(p));
        if (!uri)
            continue;
        /* Audio effects only: LV2 instruments need a MIDI→atom path the
         * engine does not have yet, and pure generators/analyzers don't fit
         * an insert chain. */
        unsigned n_in = lilv_plugin_get_num_ports_of_class(p, lph_n_audio, lph_n_input, NULL);
        unsigned n_out = lilv_plugin_get_num_ports_of_class(p, lph_n_audio, lph_n_output, NULL);
        if (n_in < 1 || n_out < 1)
            continue;
        if (!lph_required_features_ok(p))
            continue;
        if (!lph_binary_is_loadable(p))
            continue;

        LilvNode *name = lilv_plugin_get_name(p);
        const LilvPluginClass *cls = lilv_plugin_get_class(p);
        const LilvNode *label = cls ? lilv_plugin_class_get_label(cls) : NULL;

        PluginInfo *pi = g_new0(PluginInfo, 1);
        pi->format = PH_LV2;
        pi->key = g_strdup(uri);
        pi->name = g_strdup(name ? lilv_node_as_string(name) : uri);
        pi->category = g_strdup_printf("LV2|%s", label ? lilv_node_as_string(label) : "Plugin");
        pi->is_instrument = FALSE;
        *catalog = g_list_prepend(*catalog, pi);
        lilv_node_free(name);
    }
    g_mutex_unlock(&lph_world_lock);
}

/* ---- Instantiate / free ---- */

/* Resolve the plugin's HaikuUI (if any) into plain strings so no LilvUIs
 * collection has to stay alive. World lock held by the caller. */
static void lph_resolve_ui(Lv2Backend *b, LilvWorld *world)
{
    LilvNode *haiku_cls = lilv_new_uri(world, LPH_HAIKU_UI_URI);
    LilvNode *req_pred = lilv_new_uri(world, LV2_CORE__requiredFeature);
    LilvUIs *uis = lilv_plugin_get_uis(b->plugin);
    if (uis && haiku_cls) {
        LILV_FOREACH(uis, it, uis)
        {
            const LilvUI *ui = lilv_uis_get(uis, it);
            if (!lilv_ui_is_a(ui, haiku_cls))
                continue;
            /* We can only provide urid map/unmap to a UI. */
            bool ok = true;
            LilvNodes *req = lilv_world_find_nodes(world, lilv_ui_get_uri(ui), req_pred, NULL);
            if (req) {
                LILV_FOREACH(nodes, rit, req)
                {
                    const char *f = lilv_node_as_uri(lilv_nodes_get(req, rit));
                    if (!f || (strcmp(f, LV2_URID__map) != 0 && strcmp(f, LV2_URID__unmap) != 0))
                        ok = false;
                }
                lilv_nodes_free(req);
            }
            if (!ok)
                continue;
            const LilvNode *bin = lilv_ui_get_binary_uri(ui);
            const LilvNode *bundle = lilv_ui_get_bundle_uri(ui);
            if (!bin)
                continue;
            char *bin_path = lilv_file_uri_parse(lilv_node_as_uri(bin), NULL);
            char *bundle_path = bundle ? lilv_file_uri_parse(lilv_node_as_uri(bundle), NULL) : NULL;
            if (bin_path) {
                b->ui_uri = g_strdup(lilv_node_as_uri(lilv_ui_get_uri(ui)));
                b->ui_binary = g_strdup(bin_path);
                b->ui_bundle = g_strdup(bundle_path ? bundle_path : "");
            }
            lilv_free(bin_path);
            lilv_free(bundle_path);
            if (b->ui_uri)
                break;
        }
    }
    if (uis)
        lilv_uis_free(uis);
    lilv_node_free(haiku_cls);
    lilv_node_free(req_pred);
}

static void lph_backend_destroy(Lv2Backend *b);

extern "C" PluginInstance *ph_lv2_instantiate(const PluginInfo *info)
{
    if (!info || !info->key || info->format != PH_LV2)
        return NULL;

    g_mutex_lock(&lph_world_lock);
    LilvWorld *world = lph_world_get_locked();
    if (!world) {
        g_mutex_unlock(&lph_world_lock);
        return NULL;
    }

    LilvNode *uri = lilv_new_uri(world, info->key);
    const LilvPlugin *p =
        uri ? lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri) : NULL;
    lilv_node_free(uri);
    if (!p || !lph_required_features_ok(p)) {
        g_mutex_unlock(&lph_world_lock);
        g_warning("pluginhost_lv2: plugin <%s> not found or unsupported", info->key);
        return NULL;
    }

    Lv2Backend *b = g_new0(Lv2Backend, 1);
    b->plugin = p;
    b->n_ports = lilv_plugin_get_num_ports(p);
    uint32_t np = b->n_ports ? b->n_ports : 1;
    b->ctl = g_new0(float, np);
    b->shadow = g_new0(float, np);
    b->ui_last = g_new0(float, np);
    b->ain = g_new0(uint32_t, np);
    b->aout = g_new0(uint32_t, np);
    b->out_ctl = g_new0(uint32_t, np);
    b->params = g_new0(LphParam, np);
    b->atoms = g_new0(LphAtomPort, np);
    b->outA = g_new0(float, lph_maxblock);
    b->outB = g_new0(float, lph_maxblock);
    b->dummy_in = g_new0(float, lph_maxblock);
    b->dummy_out = g_new0(float, lph_maxblock);
    b->misc_bytes = (uint32_t)lph_maxblock * sizeof(float);
    if (b->misc_bytes < 8192)
        b->misc_bytes = 8192;
    b->misc = g_malloc0(b->misc_bytes);
    b->ctl_ring = zix_ring_new(NULL, LPH_RING_BYTES);
    if (!b->ctl_ring) {
        g_mutex_unlock(&lph_world_lock);
        lph_backend_destroy(b);
        return NULL;
    }
    if (zix_ring_mlock(b->ctl_ring) != ZIX_STATUS_SUCCESS)
        g_warning("pluginhost_lv2: failed to lock control ring in memory");

    float *mins = g_new0(float, np);
    float *maxs = g_new0(float, np);
    float *defs = g_new0(float, np);
    lilv_plugin_get_port_ranges_float(p, mins, maxs, defs);

    gboolean ok = TRUE;
    for (uint32_t i = 0; i < b->n_ports && ok; i++) {
        const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
        if (!port) {
            ok = FALSE;
            break;
        }
        bool is_in = lilv_port_is_a(p, port, lph_n_input);
        bool is_out = lilv_port_is_a(p, port, lph_n_output);

        if (lilv_port_is_a(p, port, lph_n_audio)) {
            if (is_in)
                b->ain[b->n_audio_in++] = i;
            if (is_out)
                b->aout[b->n_audio_out++] = i;
        } else if (lilv_port_is_a(p, port, lph_n_atom)) {
            LphAtomPort *ap = &b->atoms[b->n_atoms++];
            ap->index = i;
            ap->is_input = is_in;
            ap->capacity = b->misc_bytes;
        } else if (lilv_port_is_a(p, port, lph_n_control)) {
            float d = defs[i];
            if (d != d) /* NaN guard */
                d = 0.0f;
            b->ctl[i] = d;
            b->shadow[i] = d;
            if (!is_in) {
                b->out_ctl[b->n_out_ctl++] = i;
            } else {
                LphParam *pr = &b->params[b->n_params++];
                pr->port_index = i;
                LilvNode *pn = lilv_port_get_name(p, port);
                g_strlcpy(pr->name, pn ? lilv_node_as_string(pn) : "param", sizeof(pr->name));
                lilv_node_free(pn);
                const LilvNode *sym = lilv_port_get_symbol(p, port);
                g_strlcpy(pr->sym, sym ? lilv_node_as_string(sym) : "", sizeof(pr->sym));
                pr->min = mins[i];
                pr->max = maxs[i];
                pr->def = d;
                if (pr->max <= pr->min)
                    pr->max = pr->min + 1.0f;
                pr->toggled = lilv_port_has_property(p, port, lph_n_toggled);
                pr->integer = lilv_port_has_property(p, port, lph_n_integer);
                pr->enumeration = lilv_port_has_property(p, port, lph_n_enum);
                LilvScalePoints *sp = lilv_port_get_scale_points(p, port);
                if (sp) {
                    unsigned nsp = lilv_scale_points_size(sp);
                    if (nsp > 0) {
                        pr->points = g_new0(LphScalePoint, nsp);
                        LILV_FOREACH(scale_points, sit, sp)
                        {
                            const LilvScalePoint *pt = lilv_scale_points_get(sp, sit);
                            const LilvNode *val = lilv_scale_point_get_value(pt);
                            const LilvNode *lab = lilv_scale_point_get_label(pt);
                            if (val && pr->n_points < nsp) {
                                LphScalePoint *dst = &pr->points[pr->n_points++];
                                dst->value = lilv_node_as_float(val);
                                g_strlcpy(dst->label, lab ? lilv_node_as_string(lab) : "",
                                          sizeof(dst->label));
                            }
                        }
                    }
                    lilv_scale_points_free(sp);
                }
            }
        }
    }
    g_free(mins);
    g_free(maxs);
    g_free(defs);
    if (!ok || b->n_audio_out < 1) {
        g_mutex_unlock(&lph_world_lock);
        lph_backend_destroy(b);
        return NULL;
    }

    b->dual_mono = (b->n_audio_in == 1 && b->n_audio_out == 1);
    b->n_inst = b->dual_mono ? 2 : 1;

    b->urid_seq = lph_urid_map_cb(NULL, LV2_ATOM__Sequence);
    b->urid_chunk = lph_urid_map_cb(NULL, LV2_ATOM__Chunk);
    b->urid_float = lph_urid_map_cb(NULL, LV2_ATOM__Float);

    /* Options: block length + sample rate (many plugins require these). */
    b->opt_minblk = 1;
    b->opt_maxblk = (int32_t)lph_maxblock;
    b->opt_seqsize = (int32_t)b->misc_bytes;
    b->opt_srate = (float)lph_sr;
    const LV2_URID urid_int = lph_urid_map_cb(NULL, LV2_ATOM__Int);
    LV2_Options_Option *o = b->options;
    o[0].context = LV2_OPTIONS_INSTANCE;
    o[0].subject = 0;
    o[0].key = lph_urid_map_cb(NULL, LV2_BUF_SIZE__minBlockLength);
    o[0].size = sizeof(int32_t);
    o[0].type = urid_int;
    o[0].value = &b->opt_minblk;
    o[1] = o[0];
    o[1].key = lph_urid_map_cb(NULL, LV2_BUF_SIZE__maxBlockLength);
    o[1].value = &b->opt_maxblk;
    o[2] = o[0];
    o[2].key = lph_urid_map_cb(NULL, LV2_BUF_SIZE__sequenceSize);
    o[2].value = &b->opt_seqsize;
    o[3].context = LV2_OPTIONS_INSTANCE;
    o[3].subject = 0;
    o[3].key = lph_urid_map_cb(NULL, LV2_PARAMETERS__sampleRate);
    o[3].size = sizeof(float);
    o[3].type = b->urid_float;
    o[3].value = &b->opt_srate;
    memset(&o[4], 0, sizeof(o[4]));
    b->feat_opts.URI = LV2_OPTIONS__options;
    b->feat_opts.data = b->options;

    for (int k = 0; k < b->n_inst && ok; k++) {
        LphWorker *w = &b->workers[k];
        w->schedule.handle = w;
        w->schedule.schedule_work = lph_worker_schedule_cb;
        w->feature.URI = LV2_WORKER__schedule;
        w->feature.data = &w->schedule;

        int nf = 0;
        b->features[k][nf++] = &lph_feat_map;
        b->features[k][nf++] = &lph_feat_unmap;
        b->features[k][nf++] = &b->feat_opts;
        b->features[k][nf++] = &lph_feat_bounded;
        b->features[k][nf++] = &lph_feat_powof2;
        b->features[k][nf++] = &lph_feat_log;
        b->features[k][nf++] = &w->feature;
        b->features[k][nf] = NULL;

        b->inst[k] = lilv_plugin_instantiate(p, lph_sr, b->features[k]);
        if (!b->inst[k]) {
            ok = FALSE;
            break;
        }
        for (uint32_t a = 0; a < b->n_atoms; a++) {
            b->atoms[a].buf[k] = g_malloc0(sizeof(LV2_Atom_Sequence) + b->atoms[a].capacity);
            LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)b->atoms[a].buf[k];
            if (b->atoms[a].is_input) {
                seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
                seq->atom.type = b->urid_seq;
            } else {
                seq->atom.size = b->atoms[a].capacity;
                seq->atom.type = b->urid_chunk;
            }
        }

        /* Connect EVERY port up front: control -> its slot in the value
         * store; audio/CV -> scratch (audio re-bound per process call);
         * atom -> its own sequence buffer; anything else -> zeroed misc. */
        for (uint32_t i = 0; i < b->n_ports; i++) {
            const LilvPort *port = lilv_plugin_get_port_by_index(p, i);
            if (lilv_port_is_a(p, port, lph_n_control)) {
                lilv_instance_connect_port(b->inst[k], i, &b->ctl[i]);
            } else if (lilv_port_is_a(p, port, lph_n_audio)) {
                lilv_instance_connect_port(b->inst[k], i,
                                           lilv_port_is_a(p, port, lph_n_input) ? b->dummy_in
                                                                                : b->dummy_out);
            } else if (lilv_port_is_a(p, port, lph_n_atom)) {
                void *buf = b->misc;
                for (uint32_t a = 0; a < b->n_atoms; a++) {
                    if (b->atoms[a].index == i) {
                        buf = b->atoms[a].buf[k];
                        break;
                    }
                }
                lilv_instance_connect_port(b->inst[k], i, buf);
            } else if (lilv_port_is_a(p, port, lph_n_cv)) {
                lilv_instance_connect_port(b->inst[k], i,
                                           lilv_port_is_a(p, port, lph_n_input) ? b->dummy_in
                                                                                : b->dummy_out);
            } else {
                lilv_instance_connect_port(b->inst[k], i, b->misc);
            }
        }
        lilv_instance_activate(b->inst[k]);
        lph_worker_init(&b->workers[k], b->inst[k]);
    }
    if (!ok) {
        g_mutex_unlock(&lph_world_lock);
        lph_backend_destroy(b);
        g_warning("pluginhost_lv2: instantiate failed for <%s>", info->key);
        return NULL;
    }

    lph_resolve_ui(b, world);
    g_mutex_unlock(&lph_world_lock);

    PluginInstance *pi = g_new0(PluginInstance, 1);
    pi->format = PH_LV2;
    pi->name = g_strdup(info->name && info->name[0] ? info->name : info->key);
    pi->key = g_strdup(info->key);
    pi->category = g_strdup(info->category ? info->category : "LV2");
    pi->is_instrument = FALSE;
    pi->active = 1;
    pi->mix_q15 = 32768;
    pi->sample_rate = lph_sr;
    pi->max_block = lph_maxblock;
    pi->dry_L = g_new0(float, lph_maxblock);
    pi->dry_R = g_new0(float, lph_maxblock);
    pi->learn_arm = 0;
    pi->learn_note = -1;
    pi->lv2 = b;
    return pi;
}

static void lph_ui_unload(Lv2Backend *b)
{
    if (b->ui_handle && b->ui_desc && b->ui_desc->cleanup)
        b->ui_desc->cleanup(b->ui_handle); /* detaches + deletes the BView */
    b->ui_handle = NULL;
    b->ui_widget = NULL;
    b->ui_desc = NULL;
    if (b->ui_dl) {
        dlclose(b->ui_dl);
        b->ui_dl = NULL;
    }
}

static void lph_backend_destroy(Lv2Backend *b)
{
    if (!b)
        return;
    lph_ui_unload(b);
    for (int k = 0; k < 2; k++) {
        if (b->inst[k]) {
            lilv_instance_deactivate(b->inst[k]);
            lph_worker_destroy(&b->workers[k]); /* stop thread before freeing */
            lilv_instance_free(b->inst[k]);
            b->inst[k] = NULL;
        }
    }
    for (uint32_t a = 0; a < b->n_atoms; a++) {
        g_free(b->atoms[a].buf[0]);
        g_free(b->atoms[a].buf[1]);
    }
    zix_ring_free(b->ctl_ring);
    for (uint32_t i = 0; i < b->n_params; i++)
        g_free(b->params[i].points);
    g_free(b->params);
    g_free(b->atoms);
    g_free(b->ctl);
    g_free(b->shadow);
    g_free(b->ui_last);
    g_free(b->ain);
    g_free(b->aout);
    g_free(b->out_ctl);
    g_free(b->outA);
    g_free(b->outB);
    g_free(b->dummy_in);
    g_free(b->dummy_out);
    g_free(b->misc);
    g_free(b->ui_uri);
    g_free(b->ui_binary);
    g_free(b->ui_bundle);
    g_free(b);
}

extern "C" void ph_lv2_free(PluginInstance *inst)
{
    if (!inst)
        return;
    lph_backend_destroy(inst->lv2);
    g_free(inst->dry_L);
    g_free(inst->dry_R);
    g_free(inst->name);
    g_free(inst->key);
    g_free(inst->category);
    g_free(inst);
}

/* ---- RT processing ---- */

/* Drain queued control writes into the RT value store — RT context only. */
static inline void lph_apply_ctl_writes(Lv2Backend *b)
{
    LphCtlWrite cw;
    while (zix_ring_read_space(b->ctl_ring) >= sizeof(cw)) {
        if (zix_ring_read(b->ctl_ring, &cw, sizeof(cw)) != sizeof(cw))
            break;
        if (cw.port < b->n_ports)
            b->ctl[cw.port] = cw.value;
    }
}

/* Re-arm this instance's atom ports before run(): input ports become an empty
 * but valid LV2_Atom_Sequence; output ports advertise their capacity as a
 * Chunk. RT-safe: only touches the two header fields. */
static inline void lph_reset_atoms(Lv2Backend *b, int k)
{
    for (uint32_t a = 0; a < b->n_atoms; a++) {
        LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)b->atoms[a].buf[k];
        if (!seq)
            continue;
        if (b->atoms[a].is_input) {
            seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
            seq->atom.type = b->urid_seq;
        } else {
            seq->atom.size = b->atoms[a].capacity;
            seq->atom.type = b->urid_chunk;
        }
    }
}

/* Run the plugin in place on L/R. RT context: no alloc, no locks, no log. */
static void lph_run(Lv2Backend *b, float *L, float *R, uint32_t n)
{
    if (b->dual_mono) {
        lilv_instance_connect_port(b->inst[0], b->ain[0], L);
        lilv_instance_connect_port(b->inst[0], b->aout[0], b->outA);
        lilv_instance_connect_port(b->inst[1], b->ain[0], R);
        lilv_instance_connect_port(b->inst[1], b->aout[0], b->outB);
        lph_reset_atoms(b, 0);
        lph_reset_atoms(b, 1);
        lilv_instance_run(b->inst[0], n);
        lilv_instance_run(b->inst[1], n);
        lph_worker_apply_responses(&b->workers[0]);
        lph_worker_apply_responses(&b->workers[1]);
        memcpy(L, b->outA, (size_t)n * sizeof(float));
        memcpy(R, b->outB, (size_t)n * sizeof(float));
        return;
    }

    /* First one/two audio ports to L/R; surplus ports to scratch so run()
     * never touches an unconnected port. */
    if (b->n_audio_in > 0)
        lilv_instance_connect_port(b->inst[0], b->ain[0], L);
    if (b->n_audio_in > 1)
        lilv_instance_connect_port(b->inst[0], b->ain[1], R);
    for (int i = 2; i < b->n_audio_in; i++)
        lilv_instance_connect_port(b->inst[0], b->ain[i], b->dummy_in);
    lilv_instance_connect_port(b->inst[0], b->aout[0], b->outA);
    if (b->n_audio_out > 1)
        lilv_instance_connect_port(b->inst[0], b->aout[1], b->outB);
    for (int i = 2; i < b->n_audio_out; i++)
        lilv_instance_connect_port(b->inst[0], b->aout[i], b->dummy_out);

    lph_reset_atoms(b, 0);
    lilv_instance_run(b->inst[0], n);
    lph_worker_apply_responses(&b->workers[0]);
    memcpy(L, b->outA, (size_t)n * sizeof(float));
    memcpy(R, b->n_audio_out > 1 ? b->outB : b->outA, (size_t)n * sizeof(float));
}

extern "C" void ph_lv2_process(PluginInstance *inst, float *L, float *R, int nframes)
{
    if (!inst || !inst->lv2 || nframes <= 0)
        return;
    if (!g_atomic_int_get(&inst->active))
        return; /* bypassed: leave L/R as-is */
    Lv2Backend *b = inst->lv2;
    if (nframes > inst->max_block)
        return; /* buffer grew beyond the pre-allocation: skip, never realloc */

    int mq = g_atomic_int_get(&inst->mix_q15);
    gboolean blend = (mq < 32768) && inst->dry_L && inst->dry_R;
    if (blend) { /* stash the dry signal for the mix */
        memcpy(inst->dry_L, L, sizeof(float) * (size_t)nframes);
        memcpy(inst->dry_R, R, sizeof(float) * (size_t)nframes);
    }

    /* Re-arm FTZ/DAZ: the previous plugin (or this one) may have cleared
     * MXCSR, which would let denormals stall this plugin's run(). */
    rt_set_denormal_mode();
    gint64 t0 = ph_diag_enabled_i() ? ph_now_us_i() : 0;

    lph_apply_ctl_writes(b);
    lph_run(b, L, R, (uint32_t)nframes);

    if (t0) {
        gint64 d = ph_now_us_i() - t0;
        if (d > inst->diag_max_us)
            inst->diag_max_us = d;
    }

    /* Safety net: replace NaN/inf, clamp runaway magnitude — same clamp as
     * the VST3 path; this is what keeps a misbehaving plugin from popping
     * the speakers. */
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

extern "C" void ph_lv2_reset(PluginInstance *inst)
{
    if (!inst || !inst->lv2)
        return;
    Lv2Backend *b = inst->lv2;
    /* Deactivate→activate resets the plugin's internal DSP state. NOT RT-safe
     * (public contract: only while the RT thread is not processing this
     * instance). */
    for (int k = 0; k < b->n_inst; k++) {
        if (b->inst[k]) {
            lilv_instance_deactivate(b->inst[k]);
            lilv_instance_activate(b->inst[k]);
        }
    }
}

/* ---- Parameters (owning window thread only; values are normalized [0,1],
 * mapped linearly onto the port range) ---- */

extern "C" guint ph_lv2_param_count(PluginInstance *inst)
{
    return (inst && inst->lv2) ? inst->lv2->n_params : 0;
}

extern "C" void ph_lv2_param_name(PluginInstance *inst, guint i, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return;
    buf[0] = 0;
    if (!inst || !inst->lv2 || i >= inst->lv2->n_params)
        return;
    g_strlcpy(buf, inst->lv2->params[i].name, (gsize)buflen);
}

extern "C" float ph_lv2_param_get(PluginInstance *inst, guint i)
{
    if (!inst || !inst->lv2 || i >= inst->lv2->n_params)
        return 0.0f;
    LphParam *pr = &inst->lv2->params[i];
    float v = inst->lv2->shadow[pr->port_index];
    float norm = (v - pr->min) / (pr->max - pr->min);
    return CLAMP(norm, 0.0f, 1.0f);
}

extern "C" void ph_lv2_param_set(PluginInstance *inst, guint i, float v)
{
    if (!inst || !inst->lv2 || i >= inst->lv2->n_params)
        return;
    LphParam *pr = &inst->lv2->params[i];
    v = CLAMP(v, 0.0f, 1.0f);
    float val = pr->min + v * (pr->max - pr->min);
    if (pr->integer || pr->toggled || pr->enumeration)
        val = nearbyintf(val);
    lph_write_control(inst, pr->port_index, val);
}

extern "C" void ph_lv2_param_display(PluginInstance *inst, guint i, char *buf, int buflen)
{
    if (!buf || buflen <= 0)
        return;
    buf[0] = 0;
    if (!inst || !inst->lv2 || i >= inst->lv2->n_params)
        return;
    LphParam *pr = &inst->lv2->params[i];
    float v = inst->lv2->shadow[pr->port_index];
    if (pr->toggled) {
        g_strlcpy(buf, v > 0.5f ? "On" : "Off", (gsize)buflen);
        return;
    }
    for (uint32_t k = 0; k < pr->n_points; k++) {
        if (fabsf(pr->points[k].value - v) < 1e-6f && pr->points[k].label[0]) {
            g_strlcpy(buf, pr->points[k].label, (gsize)buflen);
            return;
        }
    }
    if (pr->integer)
        snprintf(buf, (size_t)buflen, "%d", (int)lrintf(v));
    else
        snprintf(buf, (size_t)buflen, "%.4g", (double)v);
}

extern "C" gboolean ph_lv2_param_is_stepped(PluginInstance *inst, guint i, gint *steps)
{
    if (steps)
        *steps = 1;
    if (!inst || !inst->lv2 || i >= inst->lv2->n_params)
        return FALSE;
    LphParam *pr = &inst->lv2->params[i];
    if (pr->toggled) {
        if (steps)
            *steps = 1;
        return TRUE;
    }
    if (pr->enumeration && pr->n_points > 1) {
        if (steps)
            *steps = (gint)pr->n_points - 1;
        return TRUE;
    }
    if (pr->integer) {
        gint n = (gint)lrintf(pr->max - pr->min);
        if (n >= 1) {
            if (steps)
                *steps = n;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- State (LV2 state extension via lilv; the blob stored in the .jdaw /
 * .jdpreset is the Turtle serialization string) ---- */

static const void *lph_get_port_value(const char *sym, void *user_data, uint32_t *size,
                                      uint32_t *type)
{
    PluginInstance *inst = (PluginInstance *)user_data;
    Lv2Backend *b = inst->lv2;
    for (uint32_t i = 0; i < b->n_params; i++) {
        if (!strcmp(b->params[i].sym, sym)) {
            *size = sizeof(float);
            *type = b->urid_float;
            return &b->shadow[b->params[i].port_index];
        }
    }
    *size = 0;
    *type = 0;
    return NULL;
}

static void lph_set_port_value(const char *sym, void *user_data, const void *value, uint32_t size,
                               uint32_t type)
{
    PluginInstance *inst = (PluginInstance *)user_data;
    Lv2Backend *b = inst->lv2;
    float v;
    if (type == b->urid_float && size == sizeof(float))
        v = *(const float *)value;
    else if (type == lph_urid_map_cb(NULL, LV2_ATOM__Int) && size == sizeof(int32_t))
        v = (float)*(const int32_t *)value;
    else if (type == lph_urid_map_cb(NULL, LV2_ATOM__Double) && size == sizeof(double))
        v = (float)*(const double *)value;
    else if (type == lph_urid_map_cb(NULL, LV2_ATOM__Bool) && size == sizeof(int32_t))
        v = (*(const int32_t *)value) ? 1.0f : 0.0f;
    else
        return;
    for (uint32_t i = 0; i < b->n_params; i++) {
        if (!strcmp(b->params[i].sym, sym)) {
            lph_write_control(inst, b->params[i].port_index, v);
            return;
        }
    }
}

extern "C" gboolean ph_lv2_state_save(PluginInstance *inst, void **out, gsize *out_len)
{
    if (!inst || !inst->lv2 || !out || !out_len)
        return FALSE;
    Lv2Backend *b = inst->lv2;

    g_mutex_lock(&lph_world_lock);
    LilvWorld *world = lph_world_get_locked();
    LilvState *st =
        world ? lilv_state_new_from_instance(b->plugin, b->inst[0], &lph_urid_map, NULL, NULL, NULL,
                                             NULL, lph_get_port_value, inst,
                                             LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, NULL)
              : NULL;
    char *str = NULL;
    if (st) {
        str = lilv_state_to_string(world, &lph_urid_map, &lph_urid_unmap, st, "urn:jackdaw:state",
                                   NULL);
        lilv_state_free(st);
    }
    g_mutex_unlock(&lph_world_lock);
    if (!str)
        return FALSE;

    gsize len = strlen(str);
    *out = g_memdup2(str, len);
    *out_len = len;
    lilv_free(str);
    return *out != NULL;
}

extern "C" gboolean ph_lv2_state_load(PluginInstance *inst, const void *data, gsize len)
{
    if (!inst || !inst->lv2 || !data || len == 0 || len > LPH_STATE_MAX)
        return FALSE;
    Lv2Backend *b = inst->lv2;

    /* Untrusted project-file input: bound the size (above) and NUL-terminate
     * a private copy; the Turtle parser handles the rest of the validation. */
    char *str = (char *)g_malloc(len + 1);
    memcpy(str, data, len);
    str[len] = 0;

    g_mutex_lock(&lph_world_lock);
    LilvWorld *world = lph_world_get_locked();
    LilvState *st = world ? lilv_state_new_from_string(world, &lph_urid_map, str) : NULL;
    gboolean ok = FALSE;
    if (st) {
        /* Port values are marshalled through the control ring (RT-safe). The
         * plugin-side state:interface restore() runs on this thread; that is
         * only exercised by plugins with a state interface, which — like the
         * VST3 setState path — must tolerate a concurrent-run restore. */
        for (int k = 0; k < b->n_inst; k++)
            lilv_state_restore(st, b->inst[k], lph_set_port_value, inst, 0, NULL);
        lilv_state_free(st);
        ok = TRUE;
    }
    g_mutex_unlock(&lph_world_lock);
    g_free(str);
    return ok;
}

/* ---- Native editor (HaikuUI). All calls arrive on the FX window's looper
 * with the looper locked; this TU treats the widget as an opaque pointer. ---- */

/* write_function handed to the UI: control floats go through the same
 * owning-thread write path as the generic sliders; every other protocol is
 * ignored (allow-list). Runs on the FX window looper (the view's thread). */
static void lph_ui_write_cb(LV2UI_Controller controller, uint32_t port_index, uint32_t buffer_size,
                            uint32_t protocol, const void *buffer)
{
    PluginInstance *inst = (PluginInstance *)controller;
    if (!inst || !inst->lv2)
        return;
    if (protocol == 0 && buffer_size == sizeof(float) && buffer) {
        float v = *(const float *)buffer;
        lph_write_control(inst, port_index, v);
        if (port_index < inst->lv2->n_ports)
            inst->lv2->ui_last[port_index] = v; /* no echo on the next poll */
    }
}

extern "C" void *ph_lv2_ui_create(PluginInstance *inst)
{
    if (!inst || !inst->lv2)
        return NULL;
    Lv2Backend *b = inst->lv2;
    if (b->ui_widget)
        return b->ui_widget; /* create-once: same view until destroyed */
    if (!b->ui_binary || !b->ui_uri)
        return NULL;

    b->ui_dl = dlopen(b->ui_binary, RTLD_NOW | RTLD_LOCAL);
    if (!b->ui_dl) {
        g_warning("pluginhost_lv2: dlopen '%s' failed: %s", b->ui_binary, dlerror());
        return NULL;
    }
    LV2UI_DescriptorFunction df = (LV2UI_DescriptorFunction)dlsym(b->ui_dl, "lv2ui_descriptor");
    if (df) {
        for (uint32_t i = 0;; i++) {
            const LV2UI_Descriptor *d = df(i);
            if (!d)
                break;
            if (d->URI && !strcmp(d->URI, b->ui_uri)) {
                b->ui_desc = d;
                break;
            }
        }
    }
    if (!b->ui_desc || !b->ui_desc->instantiate) {
        g_warning("pluginhost_lv2: no matching lv2ui_descriptor in '%s'", b->ui_binary);
        dlclose(b->ui_dl);
        b->ui_dl = NULL;
        b->ui_desc = NULL;
        return NULL;
    }

    const LV2_Feature *feats[] = {&lph_feat_map, &lph_feat_unmap, NULL};
    LV2UI_Widget widget = NULL;
    b->ui_handle = b->ui_desc->instantiate(b->ui_desc, inst->key, b->ui_bundle, lph_ui_write_cb,
                                           (LV2UI_Controller)inst, &widget, feats);
    if (!b->ui_handle || !widget) {
        g_warning("pluginhost_lv2: UI instantiate failed for <%s>", inst->key);
        if (b->ui_handle && b->ui_desc->cleanup)
            b->ui_desc->cleanup(b->ui_handle);
        b->ui_handle = NULL;
        dlclose(b->ui_dl);
        b->ui_dl = NULL;
        b->ui_desc = NULL;
        return NULL;
    }
    b->ui_widget = widget;
    /* Poison the last-delivered values so the first poll (after the caller
     * has attached the view) pushes every current value to the UI. */
    for (uint32_t i = 0; i < b->n_ports; i++)
        b->ui_last[i] = NAN;
    return b->ui_widget;
}

extern "C" void ph_lv2_ui_poll(PluginInstance *inst)
{
    if (!inst || !inst->lv2)
        return;
    Lv2Backend *b = inst->lv2;
    if (!b->ui_handle || !b->ui_desc || !b->ui_desc->port_event)
        return;
    /* Input parameters: the owning-thread shadow is the truth. */
    for (uint32_t i = 0; i < b->n_params; i++) {
        uint32_t port = b->params[i].port_index;
        float v = b->shadow[port];
        if (v != b->ui_last[port]) {
            b->ui_last[port] = v;
            b->ui_desc->port_event(b->ui_handle, port, sizeof(float), 0, &v);
        }
    }
    /* Output ports (meters, latency): single aligned-float read of the RT
     * store — a stale value is harmless, a torn one impossible on x86-64. */
    for (uint32_t i = 0; i < b->n_out_ctl; i++) {
        uint32_t port = b->out_ctl[i];
        float v = b->ctl[port];
        if (v != b->ui_last[port]) {
            b->ui_last[port] = v;
            b->ui_desc->port_event(b->ui_handle, port, sizeof(float), 0, &v);
        }
    }
}

extern "C" void ph_lv2_ui_destroy(PluginInstance *inst)
{
    if (!inst || !inst->lv2)
        return;
    lph_ui_unload(inst->lv2);
}
