#include "TrackAreaView.h"

#include <InterfaceDefs.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Window.h>

#include <math.h>

#include <algorithm>

#include "engine/jackdaw-engine.h"
#include "engine/tempomap.h"
#include "Messages.h"
#include "RegionGainWindow.h"
#include "TimelineView.h"
#include "TrackStripView.h"

// Empty-timeline palette (dark canvas like the Linux JackDAW wave area).
static const rgb_color kAreaBg = {45, 45, 48, 255};
static const rgb_color kHeaderBg = {58, 58, 62, 255};
static const rgb_color kBarLine = {82, 82, 88, 255};
static const rgb_color kBeatLine = {62, 62, 66, 255};
static const rgb_color kPlayhead = {230, 70, 60, 255};
static const rgb_color kRowSep = {30, 30, 32, 255};
static const rgb_color kRowSelTint = {58, 66, 82, 255};
// Waveform palette (mirrors the Linux JackDAW wave view greens + amber edges).
static const rgb_color kWaveColor = {38, 128, 51, 255};
static const rgb_color kWaveMid = {77, 179, 90, 255};
static const rgb_color kRegionEdge = {230, 220, 120, 255};
// Selected-section fill tint + rubber-band range wash (over the lane bg).
static const rgb_color kSelRegionTint = {70, 90, 130, 90};
static const rgb_color kRangeTint = {90, 110, 150, 70};
// Live-record waveform overlay (drawn while a take is being captured).
static const rgb_color kRecWave = {214, 74, 64, 255};
static const rgb_color kRecMid = {150, 46, 40, 255};
// Instrument-track MIDI note rectangles (blue) + live red record overlay.
static const rgb_color kMidiNote = {96, 156, 230, 255};
static const rgb_color kMidiRecNote = {230, 96, 88, 255};

// MIDI pitch display window on the timeline lane (the piano-roll editor in a
// later phase shows the full range): map this span across the band height.
static const int kMidiLoPitch = 24;
static const int kMidiHiPitch = 96;

// Map a MIDI pitch to a lane y (high pitch near the top of the band).
static float MidiPitchToY(int pitch, float band_y0, float band_h)
{
    if (pitch < kMidiLoPitch)
        pitch = kMidiLoPitch;
    if (pitch > kMidiHiPitch)
        pitch = kMidiHiPitch;
    float frac = (float)(pitch - kMidiLoPitch) / (float)(kMidiHiPitch - kMidiLoPitch);
    return band_y0 + band_h * (1.0f - frac);
}

// Pointer travel (px) before an armed press on a selected section becomes a move.
static const float kMoveThreshold = 3.0f;

// GObject trampolines. All emitted on the MainWindow looper (the sole mutator),
// so these run on this view's thread and may touch views directly.
static void area_tracks_changed(gpointer /*proj*/, gpointer /*track*/, gpointer user)
{
    static_cast<TrackAreaView *>(user)->RebuildStrips();
}
static void area_reordered(gpointer /*proj*/, gpointer user)
{
    static_cast<TrackAreaView *>(user)->RebuildStrips();
}
static void area_selection_changed(gpointer /*proj*/, gpointer user)
{
    static_cast<TrackAreaView *>(user)->Invalidate();
}
// A track's regions (or state) changed — repaint the lane so waveforms refresh.
static void area_track_state_changed(gpointer /*track*/, gpointer user)
{
    static_cast<TrackAreaView *>(user)->Invalidate();
}

TrackAreaView::TrackAreaView(TimelineView *timeline)
    : BView("track_area", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
      m_timeline(timeline), m_scroll_y(0.0f), m_drop_gap(-1), m_sel_track(NULL), m_selecting(false),
      m_range_active(false), m_range_start(0), m_range_end(0), m_move_armed(false), m_moving(false),
      m_move_committed(false), m_move_press_x(0.0f), m_move_press_y(0.0f), m_move_src(NULL),
      m_clipboard_midi(false), m_menu_track(NULL), m_menu_frame(0), m_added_handler(0),
      m_removed_handler(0), m_reordered_handler(0), m_selection_handler(0)
{
    m_clipboard = clip_region_list_new();
    m_midi_clipboard = midi_region_list_new();
}

TrackAreaView::~TrackAreaView()
{
    ClearMovePre();
    if (m_clipboard)
        g_ptr_array_unref(m_clipboard);
    if (m_midi_clipboard)
        g_ptr_array_unref(m_midi_clipboard);
}

void TrackAreaView::AttachedToWindow()
{
    BView::AttachedToWindow();
    m_main = BMessenger(Window());

    JackDawProject *p = m_timeline->Project();
    m_added_handler = g_signal_connect(p, "track-added", G_CALLBACK(area_tracks_changed), this);
    m_removed_handler = g_signal_connect(p, "track-removed", G_CALLBACK(area_tracks_changed), this);
    m_reordered_handler = g_signal_connect(p, "tracks-reordered", G_CALLBACK(area_reordered), this);
    m_selection_handler =
        g_signal_connect(p, "selection-changed", G_CALLBACK(area_selection_changed), this);

    RebuildStrips();
}

void TrackAreaView::DetachedFromWindow()
{
    JackDawProject *p = m_timeline->Project();
    if (m_added_handler)
        g_signal_handler_disconnect(p, m_added_handler);
    if (m_removed_handler)
        g_signal_handler_disconnect(p, m_removed_handler);
    if (m_reordered_handler)
        g_signal_handler_disconnect(p, m_reordered_handler);
    if (m_selection_handler)
        g_signal_handler_disconnect(p, m_selection_handler);
    m_added_handler = m_removed_handler = m_reordered_handler = m_selection_handler = 0;

    for (guint i = 0; i < m_strips.size() && i < m_track_state_handlers.size(); i++)
        if (m_track_state_handlers[i])
            g_signal_handler_disconnect(m_strips[i]->Track(), m_track_state_handlers[i]);
    m_track_state_handlers.clear();

    BView::DetachedFromWindow();
}

float TrackAreaView::ContentHeight() const
{
    return (float)m_timeline->Project()->tracks->len * kTimelineTrackHeight;
}

void TrackAreaView::RebuildStrips()
{
    // Drop the previous per-track region-change handlers before the strip/track
    // set changes (indices are parallel to the old m_strips order).
    for (guint i = 0; i < m_strips.size() && i < m_track_state_handlers.size(); i++)
        if (m_track_state_handlers[i])
            g_signal_handler_disconnect(m_strips[i]->Track(), m_track_state_handlers[i]);
    m_track_state_handlers.clear();

    for (TrackStripView *s : m_strips) {
        RemoveChild(s);
        delete s;
    }
    m_strips.clear();

    JackDawProject *p = m_timeline->Project();
    guint n = jackdaw_project_track_count(p);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t = jackdaw_project_get_track(p, i);
        TrackStripView *s = new TrackStripView(p, t, m_main);
        AddChild(s);
        m_strips.push_back(s);
        m_track_state_handlers.push_back(
            g_signal_connect(t, "state-changed", G_CALLBACK(area_track_state_changed), this));
    }
    RepositionStrips();
    m_timeline->UpdateVScrollBar();
    Invalidate();
}

void TrackAreaView::RepositionStrips()
{
    float y = -m_scroll_y;
    for (TrackStripView *s : m_strips) {
        s->MoveTo(0.0f, y);
        s->ResizeTo(kTimelineHeaderWidth - 1.0f, kTimelineTrackHeight - 1.0f);
        y += kTimelineTrackHeight;
    }
}

void TrackAreaView::SetScrollY(float yy)
{
    if (yy < 0.0f)
        yy = 0.0f;
    float maxy = ContentHeight() - Bounds().Height();
    if (maxy < 0.0f)
        maxy = 0.0f;
    if (yy > maxy)
        yy = maxy;
    if (yy == m_scroll_y)
        return;
    m_scroll_y = yy;
    RepositionStrips();
    Invalidate();
}

void TrackAreaView::UpdateMeters()
{
    for (TrackStripView *s : m_strips) {
        gfloat l = 0.0f, r = 0.0f;
        jackdaw_track_get_peaks(s->Track(), &l, &r);
        s->SetPeaks(l, r);
    }
}

void TrackAreaView::SetDropGap(int gap)
{
    if (gap == m_drop_gap)
        return;
    m_drop_gap = gap;
    Invalidate();
}

int TrackAreaView::RowAtY(float y) const
{
    int row = (int)((y + m_scroll_y) / kTimelineTrackHeight);
    if (row < 0 || (guint)row >= jackdaw_project_track_count(m_timeline->Project()))
        return -1;
    return row;
}

/* -----------------------------------------------------------------------
 * Region-edit helpers + operations. Ported from the Linux JackDAW timeline;
 * all run on this looper, the single project mutator, so they touch the model
 * directly. A "section" is a ClipRegion* on an audio track or a MidiRegion*
 * on an instrument track; the Sec... and SectionList... helpers dispatch on
 * the owning track's kind. Any list-mutating op clears the section selection
 * because the edit frees/rebuilds the section pointers.
 * ----------------------------------------------------------------------- */

JackDawTrack *TrackAreaView::TrackAtRow(int row) const
{
    if (row < 0)
        return NULL;
    JackDawProject *p = m_timeline->Project();
    if ((guint)row >= jackdaw_project_track_count(p))
        return NULL;
    return jackdaw_project_get_track(p, row);
}

off_t TrackAreaView::LaneXToFrame(float x) const
{
    off_t f = m_timeline->XToFrame(x - kTimelineHeaderWidth);
    return f < 0 ? 0 : f;
}

/* ---- Kind-aware section dispatch (audio ClipRegion / instrument MidiRegion),
 * the same generalization the Linux timeline uses. A section pointer is only
 * valid together with the track that owns it. ---- */

double TrackAreaView::FramesPerTick() const
{
    JackDawProject *p = m_timeline->Project();
    if (!p)
        return 0.0;
    double fpb = jackdaw_project_frames_per_beat(p, jackdaw_engine_get_sample_rate());
    return fpb / (double)JACKDAW_PPQ;
}

GPtrArray *TrackAreaView::SectionList(JackDawTrack *t) const
{
    return jackdaw_track_is_instrument(t) ? jackdaw_track_get_midi_regions(t)
                                          : jackdaw_track_get_regions(t);
}

GPtrArray *TrackAreaView::SectionListCopy(JackDawTrack *t) const
{
    return jackdaw_track_is_instrument(t) ? midi_region_list_copy(jackdaw_track_get_midi_regions(t))
                                          : clip_region_list_copy(jackdaw_track_get_regions(t));
}

off_t TrackAreaView::SecTlPos(JackDawTrack *t, void *s) const
{
    return jackdaw_track_is_instrument(t) ? ((MidiRegion *)s)->tl_pos : ((ClipRegion *)s)->tl_pos;
}

void TrackAreaView::SecSetTlPos(JackDawTrack *t, void *s, off_t pos)
{
    if (jackdaw_track_is_instrument(t))
        ((MidiRegion *)s)->tl_pos = pos;
    else
        ((ClipRegion *)s)->tl_pos = pos;
}

off_t TrackAreaView::SecEnd(JackDawTrack *t, void *s) const
{
    if (jackdaw_track_is_instrument(t))
        return midi_region_end((MidiRegion *)s, FramesPerTick());
    return clip_region_end((ClipRegion *)s);
}

void *TrackAreaView::SecListAt(JackDawTrack *t, off_t frame) const
{
    if (jackdaw_track_is_instrument(t))
        return midi_region_list_at(jackdaw_track_get_midi_regions(t), frame, FramesPerTick());
    return clip_region_list_at(jackdaw_track_get_regions(t), frame);
}

void TrackAreaView::PushRegionUndo(JackDawTrack *t)
{
    JackDawProject *p = m_timeline->Project();
    if (jackdaw_track_is_instrument(t))
        jackdaw_project_push_midi_region_undo(
            p, t, jackdaw_project_frames_per_beat(p, jackdaw_engine_get_sample_rate()));
    else
        jackdaw_project_push_region_undo(p, t);
}

bool TrackAreaView::SectionSelContains(void *s) const
{
    for (void *m : m_sel_regions)
        if (m == s)
            return true;
    return false;
}

void TrackAreaView::ClearMovePre()
{
    for (auto &kv : m_move_pre)
        if (kv.second)
            g_ptr_array_unref(kv.second);
    m_move_pre.clear();
}

void TrackAreaView::ClearSectionSelection()
{
    m_sel_track = NULL;
    m_sel_regions.clear();
    m_range_active = false;
    m_selecting = false;
    m_move_armed = false;
    m_moving = false;
    m_move_committed = false;
    m_move_src = NULL;
    m_move_orig.clear();
    ClearMovePre();
    Invalidate();
}

// Select the single section covering `frame` on `track` (highlights its whole
// span); clears the selection when the frame lands in a gap.
void TrackAreaView::SelectRegionAt(JackDawTrack *t, off_t frame)
{
    m_sel_track = NULL;
    m_sel_regions.clear();
    m_range_active = false;
    void *r = t ? SecListAt(t, frame) : NULL;
    if (r) {
        m_sel_track = t;
        m_sel_regions.push_back(r);
    }
    Invalidate();
}

void TrackAreaView::CommitTrack(JackDawTrack *t)
{
    if (jackdaw_track_is_instrument(t)) {
        midi_region_list_sort(jackdaw_track_get_midi_regions(t));
        JackDawProject *p = m_timeline->Project();
        jackdaw_track_commit_midi(
            t, jackdaw_project_frames_per_beat(p, jackdaw_engine_get_sample_rate()));
    } else {
        clip_region_list_sort(jackdaw_track_get_regions(t));
        jackdaw_track_commit_regions(t);
    }
}

bool TrackAreaView::CanPaste() const
{
    GPtrArray *cb = m_clipboard_midi ? m_midi_clipboard : m_clipboard;
    return cb && cb->len > 0;
}

// Snap a section-move delta (timeline frames): consider both the beat grid (the
// block's leading edge) and the edges of non-selected sections on the move
// track, applying the smallest correction within ~10 px.
off_t TrackAreaView::SnapMoveDelta(off_t raw_delta) const
{
    JackDawProject *p = m_timeline->Project();
    if (!p || !p->snap_enabled)
        return raw_delta;
    if (!m_sel_track || m_sel_regions.empty() || m_move_orig.empty())
        return raw_delta;

    double spp = m_timeline->Spp();
    off_t thresh = (off_t)(spp * 10.0);
    if (thresh < 1)
        thresh = 1;
    int sr = (int)jackdaw_engine_get_sample_rate();
    guint n = (guint)m_sel_regions.size();

    bool have = false;
    off_t best_corr = 0;
    off_t best_abs = thresh + 1;

    off_t lead = G_MAXINT64;
    for (guint i = 0; i < n; i++) {
        off_t e = m_move_orig[i] + raw_delta;
        if (e < lead)
            lead = e;
    }
    {
        off_t corr = jackdaw_project_snap_frame(p, lead, sr) - lead;
        off_t a = corr < 0 ? -corr : corr;
        if (a <= thresh && a < best_abs) {
            best_abs = a;
            best_corr = corr;
            have = true;
        }
    }

    GPtrArray *regs = SectionList(m_sel_track);
    for (guint i = 0; i < n; i++) {
        void *m = m_sel_regions[i];
        off_t m_len = SecEnd(m_sel_track, m) - SecTlPos(m_sel_track, m);
        off_t mine[2] = {m_move_orig[i] + raw_delta, m_move_orig[i] + raw_delta + m_len};
        for (guint j = 0; j < regs->len; j++) {
            void *o = g_ptr_array_index(regs, j);
            if (SectionSelContains(o))
                continue;
            off_t edges[2] = {SecTlPos(m_sel_track, o), SecEnd(m_sel_track, o)};
            for (int em = 0; em < 2; em++)
                for (int eo = 0; eo < 2; eo++) {
                    off_t corr = edges[eo] - mine[em];
                    off_t a = corr < 0 ? -corr : corr;
                    if (a <= thresh && a < best_abs) {
                        best_abs = a;
                        best_corr = corr;
                        have = true;
                    }
                }
        }
    }
    return have ? raw_delta + best_corr : raw_delta;
}

/* ---- Combined multi-track move undo -----------------------------------
 * A move can relocate a block across tracks, so one undo must restore every
 * track the drag touched. Pristine per-track copies are captured into
 * m_move_pre at drag start; this builds one action from them. The callbacks
 * have C linkage-compatible signatures so they can be stored as function
 * pointers in the C undo manager. */
namespace
{
struct MoveUndoCtx {
    JackDawProject *project; /* strong ref (for the MIDI tick->frame factor) */
    guint n;
    JackDawTrack **tracks; /* strong refs */
};
GPtrArray *move_track_sections_copy(JackDawTrack *t)
{
    return jackdaw_track_is_instrument(t) ? midi_region_list_copy(jackdaw_track_get_midi_regions(t))
                                          : clip_region_list_copy(jackdaw_track_get_regions(t));
}
gpointer move_undo_capture(gpointer ctx)
{
    MoveUndoCtx *c = (MoveUndoCtx *)ctx;
    GPtrArray *lists = g_ptr_array_new();
    for (guint i = 0; i < c->n; i++)
        g_ptr_array_add(lists, move_track_sections_copy(c->tracks[i]));
    return lists;
}
void move_undo_restore(gpointer ctx, gpointer state)
{
    MoveUndoCtx *c = (MoveUndoCtx *)ctx;
    GPtrArray *lists = (GPtrArray *)state;
    for (guint i = 0; i < c->n && i < lists->len; i++) {
        JackDawTrack *t = c->tracks[i];
        GPtrArray *list = (GPtrArray *)g_ptr_array_index(lists, i);
        if (jackdaw_track_is_instrument(t))
            jackdaw_track_apply_midi_regions(
                t, list,
                jackdaw_project_frames_per_beat(c->project, jackdaw_engine_get_sample_rate()));
        else
            jackdaw_track_apply_regions(t, list);
    }
}
void move_undo_free_state(gpointer state)
{
    GPtrArray *lists = (GPtrArray *)state;
    if (!lists)
        return;
    for (guint i = 0; i < lists->len; i++)
        g_ptr_array_unref((GPtrArray *)g_ptr_array_index(lists, i));
    g_ptr_array_free(lists, TRUE);
}
void move_undo_ctx_free(gpointer ctx)
{
    MoveUndoCtx *c = (MoveUndoCtx *)ctx;
    for (guint i = 0; i < c->n; i++)
        if (c->tracks[i])
            g_object_unref(c->tracks[i]);
    if (c->project)
        g_object_unref(c->project);
    g_free(c->tracks);
    g_free(c);
}
} // namespace

void TrackAreaView::PushMoveUndo()
{
    JackDawProject *p = m_timeline->Project();
    if (!p || m_move_pre.empty())
        return;
    guint n = (guint)m_move_pre.size();
    MoveUndoCtx *c = g_new0(MoveUndoCtx, 1);
    c->project = (JackDawProject *)g_object_ref(p);
    c->n = n;
    c->tracks = g_new0(JackDawTrack *, n);
    GPtrArray *saved = g_ptr_array_new();
    guint i = 0;
    for (auto &kv : m_move_pre) {
        JackDawTrack *tr = kv.first; // non-const local: avoids a qualified-cast warning
        c->tracks[i] = (JackDawTrack *)g_object_ref(tr);
        g_ptr_array_add(saved, g_ptr_array_ref(kv.second)); /* pristine pre-move copy */
        i++;
    }
    JackDawUndoAction a = {};
    a.ctx = c;
    a.saved_state = saved;
    a.capture_fn = move_undo_capture;
    a.restore_fn = move_undo_restore;
    a.free_fn = move_undo_free_state;
    a.after_fn = NULL;
    a.ctx_free_fn = move_undo_ctx_free;
    a.desc = g_strdup("Move section");
    undo_manager_push(jackdaw_project_get_undo(p), &a);
}

/* ---- Public edit operations ---- */

void TrackAreaView::SplitAtCursor()
{
    JackDawProject *p = m_timeline->Project();
    JackDawTrack *t = jackdaw_project_get_active_track(p);
    if (!t)
        return;
    off_t cur = jackdaw_engine_get_play_pos();
    if (!SecListAt(t, cur))
        return; // only split if the cut lands inside a section
    PushRegionUndo(t);
    if (jackdaw_track_is_instrument(t))
        midi_region_list_split_at(jackdaw_track_get_midi_regions(t), cur, FramesPerTick());
    else
        clip_region_list_split_at(jackdaw_track_get_regions(t), cur,
                                  (int)jackdaw_engine_get_sample_rate());
    CommitTrack(t);
    SelectRegionAt(t, cur); // focus the new right-hand region for the next edit
}

void TrackAreaView::DeleteSelection()
{
    JackDawProject *p = m_timeline->Project();
    int sr = (int)jackdaw_engine_get_sample_rate();

    // Instrument track: remove the selected MIDI sections outright.
    if (m_sel_track && jackdaw_track_is_instrument(m_sel_track) && !m_sel_regions.empty()) {
        JackDawTrack *t = m_sel_track;
        PushRegionUndo(t);
        GPtrArray *regs = jackdaw_track_get_midi_regions(t);
        for (void *s : m_sel_regions)
            g_ptr_array_remove(regs, s);
        ClearSectionSelection();
        CommitTrack(t);
        return;
    }

    if (m_sel_track && !m_sel_regions.empty()) {
        JackDawTrack *t = m_sel_track;
        guint n = (guint)m_sel_regions.size();
        std::vector<off_t> aa(n), bb(n);
        for (guint i = 0; i < n; i++) {
            aa[i] = ((ClipRegion *)m_sel_regions[i])->tl_pos;
            bb[i] = clip_region_end((ClipRegion *)m_sel_regions[i]);
        }
        jackdaw_project_push_region_undo(p, t);
        GPtrArray *regs = jackdaw_track_get_regions(t);
        for (guint i = 0; i < n; i++) // spans cached; pointers get freed below
            clip_region_list_delete_range(regs, aa[i], bb[i], sr);
        ClearSectionSelection();
        CommitTrack(t);
        return;
    }

    JackDawTrack *t = jackdaw_project_get_active_track(p);
    if (!t || jackdaw_track_is_instrument(t) || !m_range_active)
        return;
    off_t a = m_range_start, b = m_range_end;
    if (b < a) {
        off_t tmp = a;
        a = b;
        b = tmp;
    }
    jackdaw_project_push_region_undo(p, t);
    clip_region_list_delete_range(jackdaw_track_get_regions(t), a, b, sr);
    ClearSectionSelection();
    CommitTrack(t);
}

// Replace the clipboard with copies of `regs`, normalized so the earliest
// region starts at frame 0. `regs` is borrowed.
static void clipboard_set(GPtrArray *clipboard, GPtrArray *regs)
{
    g_ptr_array_set_size(clipboard, 0);
    if (regs->len == 0)
        return;
    off_t origin = ((ClipRegion *)g_ptr_array_index(regs, 0))->tl_pos;
    for (guint i = 1; i < regs->len; i++) {
        off_t pp = ((ClipRegion *)g_ptr_array_index(regs, i))->tl_pos;
        if (pp < origin)
            origin = pp;
    }
    for (guint i = 0; i < regs->len; i++) {
        ClipRegion *c = clip_region_copy((ClipRegion *)g_ptr_array_index(regs, i));
        c->tl_pos -= origin;
        g_ptr_array_add(clipboard, c);
    }
}

void TrackAreaView::CopySelection()
{
    JackDawProject *p = m_timeline->Project();
    int sr = (int)jackdaw_engine_get_sample_rate();

    // Instrument track: normalized MidiRegion copies into the MIDI clipboard.
    // Copies are frozen (auto_grow off) so a pasted section keeps its size
    // instead of re-growing to the source clip's content.
    if (m_sel_track && jackdaw_track_is_instrument(m_sel_track) && !m_sel_regions.empty()) {
        g_ptr_array_set_size(m_midi_clipboard, 0);
        off_t origin = G_MAXINT64;
        for (void *s : m_sel_regions)
            if (((MidiRegion *)s)->tl_pos < origin)
                origin = ((MidiRegion *)s)->tl_pos;
        for (void *s : m_sel_regions) {
            MidiRegion *c = midi_region_copy((MidiRegion *)s);
            c->tl_pos -= origin;
            c->auto_grow = FALSE;
            g_ptr_array_add(m_midi_clipboard, c);
        }
        m_clipboard_midi = true;
        return;
    }

    if (m_sel_track && !m_sel_regions.empty()) {
        GPtrArray *tmp = g_ptr_array_new(); // borrowed pointers, no free func
        for (void *r : m_sel_regions)
            g_ptr_array_add(tmp, r);
        clipboard_set(m_clipboard, tmp);
        g_ptr_array_free(tmp, TRUE);
        m_clipboard_midi = false;
        return;
    }

    JackDawTrack *t = jackdaw_project_get_active_track(p);
    if (!t || jackdaw_track_is_instrument(t) || !m_range_active)
        return;
    off_t a = m_range_start, b = m_range_end;
    if (b < a) {
        off_t tmp = a;
        a = b;
        b = tmp;
    }
    if (b <= a)
        return;
    // Deep-copy the track then trim outside [a,b] with the same sample-rate-aware
    // delete used by Delete Selected Area.
    GPtrArray *tmp = clip_region_list_copy(jackdaw_track_get_regions(t));
    off_t big = clip_region_list_total_frames(tmp) + 1;
    if (a > 0)
        clip_region_list_delete_range(tmp, 0, a, sr);
    if (b < big)
        clip_region_list_delete_range(tmp, b, big, sr);
    clipboard_set(m_clipboard, tmp);
    g_ptr_array_unref(tmp);
    m_clipboard_midi = false;
}

void TrackAreaView::PasteAtCursor()
{
    JackDawProject *p = m_timeline->Project();
    JackDawTrack *t = jackdaw_project_get_active_track(p);
    if (!t)
        return;

    // MIDI clipboard pastes only onto an instrument track (and vice versa).
    if (m_clipboard_midi) {
        if (!jackdaw_track_is_instrument(t) || m_midi_clipboard->len == 0)
            return;
        double fpt = FramesPerTick();
        if (fpt <= 0.0)
            return;
        off_t at = jackdaw_engine_get_play_pos();
        if (at < 0)
            at = 0;
        off_t span = 0; // normalized clipboard width
        for (guint i = 0; i < m_midi_clipboard->len; i++) {
            off_t e = midi_region_end((MidiRegion *)g_ptr_array_index(m_midi_clipboard, i), fpt);
            if (e > span)
                span = e;
        }
        GPtrArray *regs = jackdaw_track_get_midi_regions(t);
        PushRegionUndo(t);
        // Overwrite the span: split at both edges, then drop what falls inside
        // (edge positions are tick-quantized, so allow one tick of slack).
        midi_region_list_split_at(regs, at, fpt);
        midi_region_list_split_at(regs, at + span, fpt);
        off_t eps = (off_t)fpt + 1;
        for (guint i = regs->len; i > 0; i--) {
            MidiRegion *r = (MidiRegion *)g_ptr_array_index(regs, i - 1);
            if (r->tl_pos >= at - eps && midi_region_end(r, fpt) <= at + span + eps)
                g_ptr_array_remove_index(regs, i - 1);
        }
        for (guint i = 0; i < m_midi_clipboard->len; i++) {
            MidiRegion *c = midi_region_copy((MidiRegion *)g_ptr_array_index(m_midi_clipboard, i));
            c->tl_pos += at;
            g_ptr_array_add(regs, c);
        }
        ClearSectionSelection();
        CommitTrack(t);
        return;
    }

    if (jackdaw_track_is_instrument(t))
        return;
    if (!m_clipboard || m_clipboard->len == 0)
        return;
    off_t at = jackdaw_engine_get_play_pos();
    if (at < 0)
        at = 0;
    off_t span = 0; // normalized clipboard width
    for (guint i = 0; i < m_clipboard->len; i++) {
        off_t e = clip_region_end((ClipRegion *)g_ptr_array_index(m_clipboard, i));
        if (e > span)
            span = e;
    }
    int sr = (int)jackdaw_engine_get_sample_rate();
    GPtrArray *regs = jackdaw_track_get_regions(t);
    jackdaw_project_push_region_undo(p, t);
    clip_region_list_delete_range(regs, at, at + span, sr); // overwrite the span
    for (guint i = 0; i < m_clipboard->len; i++) {
        ClipRegion *c = clip_region_copy((ClipRegion *)g_ptr_array_index(m_clipboard, i));
        c->tl_pos += at;
        g_ptr_array_add(regs, c);
    }
    ClearSectionSelection();
    CommitTrack(t);
}

void TrackAreaView::GroupSelection()
{
    JackDawProject *p = m_timeline->Project();
    if (!m_sel_track || m_sel_regions.size() < 2)
        return;
    if (jackdaw_track_is_instrument(m_sel_track))
        return;
    JackDawTrack *t = m_sel_track;
    guint n = (guint)m_sel_regions.size();
    off_t *tlpos = g_new(off_t, n);
    for (guint i = 0; i < n; i++)
        tlpos[i] = ((ClipRegion *)m_sel_regions[i])->tl_pos;
    jackdaw_project_push_region_undo(p, t);
    clip_region_list_group(jackdaw_track_get_regions(t), tlpos, n,
                           (int)jackdaw_engine_get_sample_rate());
    g_free(tlpos);
    ClearSectionSelection(); // merge frees the absorbed regions
    CommitTrack(t);
}

void TrackAreaView::SetSelectionGain(float db)
{
    JackDawProject *p = m_timeline->Project();
    gfloat g = (gfloat)pow(10.0, (double)db / 20.0);
    int sr = (int)jackdaw_engine_get_sample_rate();

    if (m_sel_track && !m_sel_regions.empty()) {
        if (jackdaw_track_is_instrument(m_sel_track))
            return; // gain is audio-only
        JackDawTrack *t = m_sel_track;
        guint n = (guint)m_sel_regions.size();
        std::vector<off_t> aa(n), bb(n);
        for (guint i = 0; i < n; i++) {
            aa[i] = ((ClipRegion *)m_sel_regions[i])->tl_pos;
            bb[i] = clip_region_end((ClipRegion *)m_sel_regions[i]);
        }
        GPtrArray *regs = jackdaw_track_get_regions(t);
        jackdaw_project_push_region_undo(p, t);
        for (guint i = 0; i < n; i++)
            clip_region_list_set_gain_range(regs, aa[i], bb[i], g, sr);
        ClearSectionSelection(); // set_gain re-slices at edges
        CommitTrack(t);
        return;
    }

    JackDawTrack *t = jackdaw_project_get_active_track(p);
    if (!t || jackdaw_track_is_instrument(t) || !m_range_active)
        return;
    off_t a = m_range_start, b = m_range_end;
    if (b < a) {
        off_t tmp = a;
        a = b;
        b = tmp;
    }
    jackdaw_project_push_region_undo(p, t);
    clip_region_list_set_gain_range(jackdaw_track_get_regions(t), a, b, g, sr);
    ClearSectionSelection();
    CommitTrack(t);
}

/* Draw every track's clip-region waveforms into the lane (right of the strip
 * column). Peaks come from the AudioClip block-peak table via
 * audio_clip_get_peaks(), sampled per pixel column — the same model the Linux
 * wave view uses (3x vertical exaggeration so quiet audio stays visible). */
void TrackAreaView::DrawWaveforms(BRect lane)
{
    JackDawProject *p = m_timeline->Project();
    guint n = jackdaw_project_track_count(p);
    double spp = m_timeline->Spp();
    if (spp <= 0.0)
        return;
    off_t start = m_timeline->ViewStart();
    int jack_sr = (int)jackdaw_engine_get_sample_rate();
    int laneW = (int)lane.Width();
    off_t view0 = start;
    off_t view1 = start + (off_t)((double)laneW * spp) + 1;

    for (guint i = 0; i < n; i++) {
        float row_top = (float)i * kTimelineTrackHeight - m_scroll_y;
        float row_bot = row_top + kTimelineTrackHeight - 1.0f;
        if (row_bot < lane.top || row_top > lane.bottom)
            continue;
        JackDawTrack *t = jackdaw_project_get_track(p, i);
        GPtrArray *regs = jackdaw_track_get_regions(t);
        if (!regs)
            continue;

        float band_y0 = row_top + 3.0f;
        float band_h = kTimelineTrackHeight - 6.0f;

        for (guint ri = 0; ri < regs->len; ri++) {
            ClipRegion *r = (ClipRegion *)g_ptr_array_index(regs, ri);
            if (!r->clip || r->clip->info.frames <= 0 || r->length <= 0)
                continue;

            off_t r_tl0 = r->tl_pos;
            off_t r_tl1 = r->tl_pos + r->length;
            off_t vis0 = r_tl0 > view0 ? r_tl0 : view0;
            off_t vis1 = r_tl1 < view1 ? r_tl1 : view1;
            if (vis1 <= vis0)
                continue;

            // Selected-section highlight: a translucent wash over the region span.
            if (m_sel_track == t && SectionSelContains(r)) {
                float rx0 = lane.left + m_timeline->FrameToX(r_tl0);
                float rx1 = lane.left + m_timeline->FrameToX(r_tl1);
                if (rx0 < lane.left)
                    rx0 = lane.left;
                if (rx1 > lane.right)
                    rx1 = lane.right;
                if (rx1 > rx0) {
                    SetDrawingMode(B_OP_ALPHA);
                    SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
                    SetHighColor(kSelRegionTint);
                    FillRect(BRect(rx0, row_top, rx1, row_bot));
                    SetDrawingMode(B_OP_COPY);
                }
            }

            int px0 = (int)(((double)vis0 - start) / spp);
            int px1 = (int)(((double)vis1 - start) / spp) + 1;
            if (px0 < 0)
                px0 = 0;
            if (px1 > laneW)
                px1 = laneW;
            int npx = px1 - px0;
            if (npx <= 0)
                continue;

            int clip_sr = r->clip->info.samplerate;
            double ratio = (clip_sr > 0 && jack_sr > 0) ? (double)clip_sr / (double)jack_sr : 1.0;
            sf_count_t cf0 = r->file_in + (sf_count_t)((vis0 - r_tl0) * ratio);
            sf_count_t cf1 = r->file_in + (sf_count_t)((vis1 - r_tl0) * ratio);
            if (cf1 <= cf0)
                cf1 = cf0 + 1;
            if (cf1 > r->clip->info.frames)
                cf1 = r->clip->info.frames;
            if (cf0 < 0)
                cf0 = 0;
            if (cf1 <= cf0)
                continue;

            int ch = r->clip->info.channels;
            int draw_ch = ch > 2 ? 2 : ch;
            float gain = r->gain;

            gfloat *out_min = g_new(gfloat, (gsize)npx * ch);
            gfloat *out_max = g_new(gfloat, (gsize)npx * ch);
            audio_clip_get_peaks(r->clip, cf0, cf1, npx, out_min, out_max);

            for (int c = 0; c < draw_ch; c++) {
                float sub_h = band_h / (float)draw_ch;
                float sub_y0 = band_y0 + (float)c * sub_h;
                float mid_y = sub_y0 + sub_h / 2.0f;
                float half = (sub_h / 2.0f) * 3.0f; /* vertical exaggeration */

                BeginLineArray(npx);
                for (int x = 0; x < npx; x++) {
                    float mn = out_min[x * ch + c] * gain;
                    float mx = out_max[x * ch + c] * gain;
                    if (mn > mx)
                        continue;
                    float y_top = mid_y - mx * half;
                    float y_bot = mid_y - mn * half;
                    if (y_top < sub_y0)
                        y_top = sub_y0;
                    if (y_bot > sub_y0 + sub_h)
                        y_bot = sub_y0 + sub_h;
                    float sx = lane.left + (float)(px0 + x) + 0.5f;
                    AddLine(BPoint(sx, y_top), BPoint(sx, y_bot), kWaveColor);
                }
                EndLineArray();

                SetHighColor(kWaveMid);
                StrokeLine(BPoint(lane.left + (float)px0, mid_y),
                           BPoint(lane.left + (float)px1, mid_y));
            }
            g_free(out_min);
            g_free(out_max);

            /* Region boundary edges — both start and end, so a gap left by a
             * deleted neighbour still reads as two real edges. Skip the first
             * region's start at timeline 0. */
            SetHighColor(kRegionEdge);
            if (r_tl0 > 0) {
                float bx = lane.left + m_timeline->FrameToX(r_tl0);
                if (bx >= lane.left && bx <= lane.right)
                    StrokeLine(BPoint(bx, row_top), BPoint(bx, row_bot));
            }
            float ex = lane.left + m_timeline->FrameToX(r_tl1);
            if (ex >= lane.left && ex <= lane.right)
                StrokeLine(BPoint(ex, row_top), BPoint(ex, row_bot));
        }
    }
}

void TrackAreaView::DrawRecordOverlay(BRect lane)
{
    if (!jackdaw_engine_is_recording())
        return;
    JackDawProject *p = m_timeline->Project();
    guint n = jackdaw_project_track_count(p);
    double spp = m_timeline->Spp();
    if (spp <= 0.0)
        return;
    off_t start = m_timeline->ViewStart();
    off_t view1 = start + (off_t)((double)lane.Width() * spp) + 1;

    for (guint i = 0; i < n; i++) {
        float row_top = (float)i * kTimelineTrackHeight - m_scroll_y;
        float row_bot = row_top + kTimelineTrackHeight - 1.0f;
        if (row_bot < lane.top || row_top > lane.bottom)
            continue;
        JackDawTrack *t = jackdaw_project_get_track(p, i);
        if (!t || !jackdaw_track_is_armed(t))
            continue;

        // Snapshot the racy RT-written fields once (count only ever grows).
        gint count = t->rec_peak_count;
        gint block = t->rec_peak_block;
        const gfloat *peaks = (const gfloat *)t->rec_peak_buf;
        if (!peaks || count <= 0 || block <= 0)
            continue;
        off_t rec0 = t->rec_start_frame;

        // Only walk the buckets that fall inside the visible span.
        gint kfirst = (start > rec0) ? (gint)((start - rec0) / block) : 0;
        gint klast = (gint)((view1 - rec0) / block) + 1;
        if (kfirst < 0)
            kfirst = 0;
        if (klast > count)
            klast = count;

        float band_y0 = row_top + 3.0f;
        float band_h = kTimelineTrackHeight - 6.0f;
        float mid_y = band_y0 + band_h / 2.0f;
        float half = (band_h / 2.0f) * 3.0f;

        SetHighColor(kRecMid);
        StrokeLine(BPoint(lane.left, mid_y), BPoint(lane.right, mid_y));

        SetHighColor(kRecWave);
        for (gint k = kfirst; k < klast; k++) {
            off_t f0 = rec0 + (off_t)k * block;
            float x0 = lane.left + m_timeline->FrameToX(f0);
            float x1 = lane.left + m_timeline->FrameToX(f0 + block);
            if (x1 < lane.left || x0 > lane.right)
                continue;
            if (x0 < lane.left)
                x0 = lane.left;
            if (x1 > lane.right)
                x1 = lane.right;
            if (x1 < x0 + 1.0f)
                x1 = x0 + 1.0f;
            float mn = peaks[k * 2];
            float mx = peaks[k * 2 + 1];
            float y_top = mid_y - mx * half;
            float y_bot = mid_y - mn * half;
            if (y_top < band_y0)
                y_top = band_y0;
            if (y_bot > band_y0 + band_h)
                y_bot = band_y0 + band_h;
            if (y_bot < y_top + 1.0f)
                y_bot = y_top + 1.0f;
            FillRect(BRect(x0, y_top, x1, y_bot));
        }
    }
}

/* Draw every instrument track's MIDI notes as rectangles: for each region,
 * emit the notes of its clip that fall in the region window, mapping tick->frame
 * ->x with the project tempo and pitch->y across the band. Region edges are
 * drawn like audio regions so splits/moves read the same. */
void TrackAreaView::DrawMidiNotes(BRect lane)
{
    JackDawProject *p = m_timeline->Project();
    guint n = jackdaw_project_track_count(p);
    double spp = m_timeline->Spp();
    if (spp <= 0.0)
        return;

    TempoMap tm;
    tempomap_from_project(&tm, p, jackdaw_engine_get_sample_rate());
    double fpb = tempomap_frames_per_beat(&tm);
    double f_per_tick = (fpb > 0.0) ? fpb / (double)JACKDAW_PPQ : 0.0;
    if (f_per_tick <= 0.0)
        return;

    off_t start = m_timeline->ViewStart();
    off_t view1 = start + (off_t)((double)lane.Width() * spp) + 1;

    for (guint i = 0; i < n; i++) {
        float row_top = (float)i * kTimelineTrackHeight - m_scroll_y;
        float row_bot = row_top + kTimelineTrackHeight - 1.0f;
        if (row_bot < lane.top || row_top > lane.bottom)
            continue;
        JackDawTrack *t = jackdaw_project_get_track(p, i);
        if (!t || !jackdaw_track_is_instrument(t))
            continue;
        GPtrArray *regs = jackdaw_track_get_midi_regions(t);
        if (!regs)
            continue;

        float band_y0 = row_top + 3.0f;
        float band_h = kTimelineTrackHeight - 6.0f;

        for (guint ri = 0; ri < regs->len; ri++) {
            MidiRegion *r = (MidiRegion *)g_ptr_array_index(regs, ri);
            if (!r->clip || !r->clip->notes)
                continue;
            off_t r_tl0 = r->tl_pos;
            off_t r_tl1 = midi_region_end(r, f_per_tick);
            if (r_tl1 <= start || r_tl0 >= view1)
                continue;

            // Selected-section highlight: same translucent wash as audio regions.
            if (m_sel_track == t && SectionSelContains(r)) {
                float rx0 = lane.left + m_timeline->FrameToX(r_tl0);
                float rx1 = lane.left + m_timeline->FrameToX(r_tl1);
                if (rx0 < lane.left)
                    rx0 = lane.left;
                if (rx1 > lane.right)
                    rx1 = lane.right;
                if (rx1 > rx0) {
                    SetDrawingMode(B_OP_ALPHA);
                    SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
                    SetHighColor(kSelRegionTint);
                    FillRect(BRect(rx0, row_top, rx1, row_bot));
                    SetDrawingMode(B_OP_COPY);
                }
            }

            guint32 win0 = r->clip_in;
            guint32 win1 = r->clip_in + r->length;
            GArray *notes = r->clip->notes;
            SetHighColor(kMidiNote);
            for (guint ni = 0; ni < notes->len; ni++) {
                MidiNote *nt = &g_array_index(notes, MidiNote, ni);
                if (nt->velocity == 0)
                    continue;
                if (nt->start < win0 || nt->start >= win1)
                    continue;
                guint32 note_end = nt->start + nt->length;
                if (note_end > win1)
                    note_end = win1;
                off_t on_f = r_tl0 + (off_t)((double)(nt->start - win0) * f_per_tick + 0.5);
                off_t off_f = r_tl0 + (off_t)((double)(note_end - win0) * f_per_tick + 0.5);
                if (off_f < start || on_f > view1)
                    continue;
                float x0 = lane.left + m_timeline->FrameToX(on_f);
                float x1 = lane.left + m_timeline->FrameToX(off_f);
                if (x1 < x0 + 2.0f)
                    x1 = x0 + 2.0f;
                if (x0 < lane.left)
                    x0 = lane.left;
                if (x1 > lane.right)
                    x1 = lane.right;
                float y = MidiPitchToY(nt->pitch, band_y0, band_h);
                FillRect(BRect(x0, y - 1.0f, x1, y + 1.0f));
            }

            // Region boundary edges (skip the first region's start at frame 0).
            SetHighColor(kRegionEdge);
            if (r_tl0 > 0) {
                float bx = lane.left + m_timeline->FrameToX(r_tl0);
                if (bx >= lane.left && bx <= lane.right)
                    StrokeLine(BPoint(bx, row_top), BPoint(bx, row_bot));
            }
            float ex = lane.left + m_timeline->FrameToX(r_tl1);
            if (ex >= lane.left && ex <= lane.right)
                StrokeLine(BPoint(ex, row_top), BPoint(ex, row_bot));
        }
    }
}

/* Live red MIDI note overlay for any instrument track currently capturing: the
 * engine peeks its capture ring and returns in-progress notes in absolute frames
 * (held notes extended to the playhead). */
void TrackAreaView::DrawMidiRecOverlay(BRect lane)
{
    if (!jackdaw_engine_is_recording())
        return;
    JackDawProject *p = m_timeline->Project();
    guint n = jackdaw_project_track_count(p);
    double spp = m_timeline->Spp();
    if (spp <= 0.0)
        return;
    off_t start = m_timeline->ViewStart();
    off_t view1 = start + (off_t)((double)lane.Width() * spp) + 1;

    for (guint i = 0; i < n; i++) {
        float row_top = (float)i * kTimelineTrackHeight - m_scroll_y;
        float row_bot = row_top + kTimelineTrackHeight - 1.0f;
        if (row_bot < lane.top || row_top > lane.bottom)
            continue;
        JackDawTrack *t = jackdaw_project_get_track(p, i);
        if (!t || !jackdaw_track_is_instrument(t) || !jackdaw_track_is_armed(t))
            continue;

        guint cnt = 0;
        const JackDawRecNote *rn = jackdaw_engine_rec_preview(t, &cnt);
        if (!rn || cnt == 0)
            continue;

        float band_y0 = row_top + 3.0f;
        float band_h = kTimelineTrackHeight - 6.0f;
        SetHighColor(kMidiRecNote);
        for (guint k = 0; k < cnt; k++) {
            if (rn[k].end_frame < start || rn[k].start_frame > view1)
                continue;
            float x0 = lane.left + m_timeline->FrameToX(rn[k].start_frame);
            float x1 = lane.left + m_timeline->FrameToX(rn[k].end_frame);
            if (x1 < x0 + 2.0f)
                x1 = x0 + 2.0f;
            if (x0 < lane.left)
                x0 = lane.left;
            if (x1 > lane.right)
                x1 = lane.right;
            float y = MidiPitchToY(rn[k].pitch, band_y0, band_h);
            FillRect(BRect(x0, y - 1.0f, x1, y + 1.0f));
        }
    }
}

void TrackAreaView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect bounds = Bounds();

    // Strip column background (strips draw over their own rows on top of this).
    BRect header = bounds;
    header.right = kTimelineHeaderWidth - 1;
    SetHighColor(kHeaderBg);
    FillRect(header);

    BRect lane = bounds;
    lane.left = kTimelineHeaderWidth;
    SetHighColor(kAreaBg);
    FillRect(lane);

    JackDawProject *p = m_timeline->Project();
    guint n = jackdaw_project_track_count(p);

    // Per-row selection tint + bottom separators (lane side only).
    for (guint i = 0; i < n; i++) {
        float top = (float)i * kTimelineTrackHeight - m_scroll_y;
        float bot = top + kTimelineTrackHeight - 1.0f;
        if (bot < lane.top || top > lane.bottom)
            continue;
        if (jackdaw_project_is_selected(p, jackdaw_project_get_track(p, i))) {
            SetHighColor(kRowSelTint);
            FillRect(BRect(lane.left, top, lane.right, bot));
        }
        SetHighColor(kRowSep);
        StrokeLine(BPoint(lane.left, bot), BPoint(lane.right, bot));
    }

    // Beat/bar grid (when enabled) across the whole lane height.
    if (p->grid_enabled) {
        TempoMap tm;
        tempomap_from_project(&tm, p, jackdaw_engine_get_sample_rate());
        double fpbeat = tempomap_frames_per_beat(&tm);
        double spp = m_timeline->Spp();
        if (fpbeat > 0.0 && spp > 0.0) {
            off_t start = m_timeline->ViewStart();
            off_t end = m_timeline->XToFrame(lane.Width());
            gboolean draw_beats = (fpbeat / spp) >= 7.0;
            gint64 first_beat = (gint64)((double)start / fpbeat);
            guint bpb = tm.beats_per_bar ? tm.beats_per_bar : 4;
            for (gint64 b = first_beat;; b++) {
                off_t frame = (off_t)((double)b * fpbeat + 0.5);
                if (frame > end)
                    break;
                if (frame < start)
                    continue;
                gboolean is_bar = (b % bpb) == 0;
                if (!is_bar && !draw_beats)
                    continue;
                float x = lane.left + m_timeline->FrameToX(frame);
                SetHighColor(is_bar ? kBarLine : kBeatLine);
                StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
            }
        }
    }

    // Rubber-band range selection: a translucent time band across all lanes.
    if (m_range_active) {
        off_t a = m_range_start, b = m_range_end;
        if (b < a) {
            off_t tmp = a;
            a = b;
            b = tmp;
        }
        float rx0 = lane.left + m_timeline->FrameToX(a);
        float rx1 = lane.left + m_timeline->FrameToX(b);
        if (rx0 < lane.left)
            rx0 = lane.left;
        if (rx1 > lane.right)
            rx1 = lane.right;
        if (rx1 > rx0) {
            SetDrawingMode(B_OP_ALPHA);
            SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            SetHighColor(kRangeTint);
            FillRect(BRect(rx0, lane.top, rx1, lane.bottom));
            SetDrawingMode(B_OP_COPY);
        }
    }

    // Clip-region waveforms (drawn over the grid, under the playhead).
    DrawWaveforms(lane);

    // Instrument-track MIDI note rectangles (over the grid, under the playhead).
    DrawMidiNotes(lane);

    // Live red waveform for any take currently being captured (over the clips).
    DrawRecordOverlay(lane);

    // Live red MIDI notes for any instrument take currently being captured.
    DrawMidiRecOverlay(lane);

    // Playhead line.
    float x = lane.left + m_timeline->FrameToX(jackdaw_engine_get_play_pos());
    if (x >= lane.left && x <= lane.right) {
        SetHighColor(kPlayhead);
        StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
    }

    // Drop-insertion indicator during a track drag-reorder. Drawn across the lane
    // (right of the strip column): the opaque strip children cover the header
    // column, so the line would otherwise be hidden behind them.
    if (m_drop_gap >= 0) {
        float gy = (float)m_drop_gap * kTimelineTrackHeight - m_scroll_y;
        SetHighColor(60, 150, 230);
        FillRect(BRect(lane.left, gy - 1.0f, lane.right, gy + 1.0f));
    }
}

void TrackAreaView::MouseDown(BPoint where)
{
    JackDawProject *p = m_timeline->Project();
    if (where.x < kTimelineHeaderWidth) {
        // Empty strip column below the last track: clear the selection.
        if (RowAtY(where.y) < 0)
            jackdaw_project_clear_selection(p);
        return;
    }

    int32 buttons = 0, mods = 0, clicks = 1;
    BMessage *msg = Window() ? Window()->CurrentMessage() : NULL;
    if (msg) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("modifiers", &mods);
        msg->FindInt32("clicks", &clicks);
    }
    bool ctrl = (mods & (B_COMMAND_KEY | B_CONTROL_KEY)) != 0;
    bool secondary = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;

    int row = RowAtY(where.y);
    JackDawTrack *t = TrackAtRow(row);
    off_t frame = LaneXToFrame(where.x);

    // Double-click on an instrument lane opens the track's piano-roll editor.
    if (!secondary && clicks == 2 && t && jackdaw_track_is_instrument(t)) {
        BMessage open(MSG_MIDI_OPEN_EDITOR);
        open.AddPointer("track", t);
        Window()->PostMessage(&open);
        return;
    }
    void *r = t ? SecListAt(t, frame) : NULL;

    // Unify strip + timeline selection: a plain click selects just this track;
    // Ctrl+click keeps any multi-track selection and only makes this active.
    if (t) {
        if (!secondary && ctrl)
            jackdaw_project_set_active_track(p, t);
        else
            jackdaw_project_select_single(p, t);
    }

    // Right-click: context menu acting on the section under the pointer. Keep an
    // existing multi-selection if the click landed inside one of its members.
    if (secondary) {
        m_menu_track = t;
        m_menu_frame = frame;
        if (!(r && m_sel_track == t && SectionSelContains(r)))
            SelectRegionAt(t, frame);
        ShowContextMenu(t, frame, ConvertToScreen(where));
        return;
    }

    // Ctrl+left toggles a section in the (single-track) multi-selection.
    if (ctrl) {
        if (r) {
            if (m_sel_track != t) {
                m_sel_regions.clear();
                m_range_active = false;
                m_sel_track = t;
            }
            if (SectionSelContains(r))
                m_sel_regions.erase(std::remove(m_sel_regions.begin(), m_sel_regions.end(), r),
                                    m_sel_regions.end());
            else
                m_sel_regions.push_back(r);
            if (m_sel_regions.empty())
                m_sel_track = NULL;
        }
        Invalidate();
        return;
    }

    // Plain press on a region → select it (keeping an existing multi-selection
    // that already contains it) and arm a move-drag, so a single click-drag
    // moves the clip. A press that never travels falls through in MouseUp to a
    // plain seek, so clicking a clip still drops the play head into it.
    if (r) {
        if (!(m_sel_track == t && SectionSelContains(r)))
            SelectRegionAt(t, frame);
        m_move_armed = true;
        m_moving = false;
        m_move_committed = false;
        m_move_press_x = where.x;
        m_move_press_y = where.y;
        m_move_src = t;
        m_move_orig.clear();
        for (void *s : m_sel_regions)
            m_move_orig.push_back(SecTlPos(t, s));
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }

    // Empty lane (a gap between clips): locate the play head and start a
    // potential rubber-band time-range (drives delete/copy/gain over a span).
    ClearSectionSelection();
    m_timeline->LocateTo(frame);
    m_selecting = true;
    m_range_active = false;
    m_range_start = frame;
    m_range_end = frame;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}

void TrackAreaView::MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage)
{
    (void)code;
    (void)dragMessage;

    // An armed press becomes a real move once the pointer travels past a small
    // threshold in either axis (horizontal = slide, vertical = relocate track).
    if (m_move_armed && !m_moving) {
        if (fabs(where.x - m_move_press_x) > kMoveThreshold ||
            fabs(where.y - m_move_press_y) > kMoveThreshold)
            m_moving = true;
        else
            return;
    }

    if (m_moving && !m_sel_regions.empty() && !m_move_orig.empty() && m_sel_track) {
        // Vertical: relocate the block to the track under the pointer, if it is
        // a different track of the same kind (audio<->audio, MIDI<->MIDI). The
        // section pointers are stolen between the two lists so the selection (and
        // m_move_orig, which drives the horizontal offset below) stay valid.
        JackDawTrack *tgt = TrackAtRow(RowAtY(where.y));
        if (tgt && tgt != m_sel_track &&
            jackdaw_track_is_instrument(tgt) == jackdaw_track_is_instrument(m_sel_track)) {
            if (m_move_pre.find(m_sel_track) == m_move_pre.end())
                m_move_pre[m_sel_track] = SectionListCopy(m_sel_track);
            if (m_move_pre.find(tgt) == m_move_pre.end())
                m_move_pre[tgt] = SectionListCopy(tgt);
            m_move_committed = true;

            GPtrArray *from = SectionList(m_sel_track);
            GPtrArray *to = SectionList(tgt);
            for (void *s : m_sel_regions) {
                guint idx;
                if (g_ptr_array_find(from, s, &idx)) {
                    g_ptr_array_steal_index_fast(from, idx);
                    g_ptr_array_add(to, s);
                }
            }
            m_sel_track = tgt;
        }

        double spp = m_timeline->Spp();
        off_t raw = (off_t)((where.x - m_move_press_x) * spp);
        off_t delta = SnapMoveDelta(raw);
        guint n = (guint)m_sel_regions.size();

        off_t min_orig = G_MAXINT64;
        for (guint i = 0; i < n; i++)
            if (m_move_orig[i] < min_orig)
                min_orig = m_move_orig[i];
        if (min_orig + delta < 0)
            delta = -min_orig; // no section starts before 0

        if (delta != 0 && !m_move_committed) {
            if (m_move_pre.find(m_sel_track) == m_move_pre.end())
                m_move_pre[m_sel_track] = SectionListCopy(m_sel_track);
            m_move_committed = true;
        }
        for (guint i = 0; i < n; i++)
            SecSetTlPos(m_sel_track, m_sel_regions[i], m_move_orig[i] + delta);
        Invalidate();
        return;
    }

    if (!m_selecting)
        return;
    off_t frame = LaneXToFrame(where.x);
    m_range_end = frame;
    off_t span = frame - m_range_start;
    if (span < 0)
        span = -span;
    double spp = m_timeline->Spp();
    if ((double)span > spp * 3.0) // a few pixels' worth => a real range
        m_range_active = true;
    Invalidate();
}

void TrackAreaView::MouseUp(BPoint where)
{
    // Finalize a move: re-sort + republish every touched track, one combined undo.
    if (m_moving) {
        m_moving = false;
        m_move_armed = false;
        if (!m_move_pre.empty()) {
            for (auto &kv : m_move_pre)
                CommitTrack(kv.first);
            PushMoveUndo();
            ClearMovePre();
        } else if (m_sel_track) {
            CommitTrack(m_sel_track);
        }
        m_move_src = NULL;
        m_move_orig.clear();
        Invalidate();
        return;
    }

    // Armed but never dragged → a plain click on a selected section: seek there
    // and keep the selection.
    if (m_move_armed) {
        m_move_armed = false;
        m_move_orig.clear();
        m_timeline->LocateTo(LaneXToFrame(where.x));
        return;
    }

    // A rubber-band drag that never grew is a plain click: select the section
    // under the pointer (a real drag already set m_range_active).
    if (m_selecting) {
        m_selecting = false;
        if (!m_range_active)
            SelectRegionAt(TrackAtRow(RowAtY(where.y)), m_range_start);
    }
}

void TrackAreaView::FrameResized(float w, float h)
{
    BView::FrameResized(w, h);
    m_timeline->UpdateVScrollBar();
    SetScrollY(m_scroll_y); // re-clamp against the new visible height
}

// Lane right-click popup, built fresh so item sensitivity is current. Runs
// synchronously (menu.Go blocks this looper), so choices dispatch inline; the
// gain choice spawns a modal dialog that posts MSG_REGION_SET_GAIN back here.
void TrackAreaView::ShowContextMenu(JackDawTrack *t, off_t frame, BPoint screen_where)
{
    (void)frame;
    bool sel_audio = m_sel_track && !jackdaw_track_is_instrument(m_sel_track);
    bool op_audio = sel_audio || (!m_sel_track && t && !jackdaw_track_is_instrument(t));
    bool have_sel = !m_sel_regions.empty() || m_range_active;
    bool can_group = m_sel_regions.size() >= 2 && sel_audio;
    // Delete/Copy act on a section selection of either kind; only the
    // rubber-band range form is audio-only. Paste needs a kind-matching track.
    bool sec_ops = !m_sel_regions.empty() || (m_range_active && op_audio);
    bool can_paste = CanPaste() && t && jackdaw_track_is_instrument(t) == m_clipboard_midi;

    BPopUpMenu menu("region_context", false, false);
    BMenuItem *mi_piano = NULL;
    if (t && jackdaw_track_is_instrument(t)) {
        mi_piano = new BMenuItem("Open Piano Roll", NULL);
        menu.AddItem(mi_piano);
        menu.AddSeparatorItem();
    }
    BMenuItem *mi_split = new BMenuItem("Split at Playhead", NULL);
    mi_split->SetEnabled(t != NULL);
    menu.AddItem(mi_split);
    BMenuItem *mi_del = new BMenuItem("Delete Selected Area", NULL);
    mi_del->SetEnabled(sec_ops);
    menu.AddItem(mi_del);
    BMenuItem *mi_copy = new BMenuItem("Copy", NULL);
    mi_copy->SetEnabled(sec_ops);
    menu.AddItem(mi_copy);
    BMenuItem *mi_paste = new BMenuItem("Paste at Playhead", NULL);
    mi_paste->SetEnabled(can_paste);
    menu.AddItem(mi_paste);
    BMenuItem *mi_gain = new BMenuItem("Set Selection Gain…", NULL);
    mi_gain->SetEnabled(have_sel && op_audio);
    menu.AddItem(mi_gain);
    BMenuItem *mi_group = new BMenuItem("Group Sections", NULL);
    mi_group->SetEnabled(can_group);
    menu.AddItem(mi_group);
    BMenuItem *mi_delreg = new BMenuItem("Delete Region", NULL);
    mi_delreg->SetEnabled(t != NULL);
    menu.AddItem(mi_delreg);
    BMenuItem *mi_clrloop = new BMenuItem("Clear Loop Region", NULL);
    mi_clrloop->SetEnabled(jackdaw_engine_has_loop_region());
    menu.AddItem(mi_clrloop);

    BMenuItem *mi_deltrack = NULL;
    if (t) {
        menu.AddSeparatorItem();
        mi_deltrack = new BMenuItem("Delete Track", NULL);
        menu.AddItem(mi_deltrack);
    }

    BMenuItem *chosen = menu.Go(screen_where, false, true);
    if (!chosen)
        return;
    if (mi_piano && chosen == mi_piano) {
        BMessage open(MSG_MIDI_OPEN_EDITOR);
        open.AddPointer("track", t);
        Window()->PostMessage(&open);
        return;
    }
    if (chosen == mi_split)
        SplitAtCursor();
    else if (chosen == mi_del)
        DeleteSelection();
    else if (chosen == mi_copy)
        CopySelection();
    else if (chosen == mi_paste)
        PasteAtCursor();
    else if (chosen == mi_gain) {
        // Seed the dialog with the current gain of the first target region.
        ClipRegion *cur = NULL;
        if (!m_sel_regions.empty() && sel_audio) {
            cur = (ClipRegion *)m_sel_regions[0];
        } else if (op_audio) {
            JackDawTrack *gt = m_sel_track ? m_sel_track : t;
            off_t a = m_range_start < m_range_end ? m_range_start : m_range_end;
            if (gt)
                cur = clip_region_list_at(jackdaw_track_get_regions(gt), a);
        }
        double cur_db =
            (cur && cur->gain > 0.0f) ? CLAMP(20.0 * log10((double)cur->gain), -25.0, 25.0) : 0.0;
        RegionGainWindow *dlg = new RegionGainWindow(BMessenger(this), cur_db);
        dlg->Show();
    } else if (chosen == mi_group) {
        GroupSelection();
    } else if (chosen == mi_delreg) {
        if (m_menu_track) {
            PushRegionUndo(m_menu_track);
            if (jackdaw_track_is_instrument(m_menu_track)) {
                GPtrArray *regs = jackdaw_track_get_midi_regions(m_menu_track);
                MidiRegion *r = midi_region_list_at(regs, m_menu_frame, FramesPerTick());
                if (r)
                    g_ptr_array_remove(regs, r);
            } else {
                clip_region_list_remove_at(jackdaw_track_get_regions(m_menu_track), m_menu_frame);
            }
            ClearSectionSelection();
            CommitTrack(m_menu_track);
        }
    } else if (chosen == mi_clrloop) {
        jackdaw_engine_set_loop_range(0, 0);
        jackdaw_engine_set_loop_enabled(FALSE);
        m_timeline->InvalidateAll();
    } else if (chosen == mi_deltrack && t) {
        BMessage del(MSG_TRACK_DELETE_SLOT);
        del.AddInt32("slot", (int32)t->slot);
        m_main.SendMessage(&del);
    }
}

void TrackAreaView::MessageReceived(BMessage *message)
{
    if (message->what == MSG_REGION_SET_GAIN) {
        float db = 0.0f;
        if (message->FindFloat("db", &db) == B_OK)
            SetSelectionGain(db);
        return;
    }
    if (message->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            BPoint where;
            uint32 buttons;
            GetMouse(&where, &buttons, false);
            int32 mods = modifiers();
            if (mods & (B_COMMAND_KEY | B_CONTROL_KEY)) {
                // Command/Ctrl + wheel = horizontal zoom.
                m_timeline->ZoomBy(dy > 0 ? 1.25 : 0.8, where.x - kTimelineHeaderWidth);
            } else {
                // Plain wheel = vertical track scroll.
                m_timeline->ScrollTracksBy(dy * kTimelineTrackHeight * 0.5f);
            }
        }
        return;
    }
    BView::MessageReceived(message);
}
