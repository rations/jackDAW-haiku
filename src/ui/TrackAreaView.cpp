#include "TrackAreaView.h"

#include <InterfaceDefs.h>
#include <Window.h>

#include "engine/jackdaw-engine.h"
#include "engine/tempomap.h"
#include "Messages.h"
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
      m_timeline(timeline), m_scroll_y(0.0f), m_dragging(false), m_drop_gap(-1), m_added_handler(0),
      m_removed_handler(0), m_reordered_handler(0), m_selection_handler(0)
{
}

TrackAreaView::~TrackAreaView()
{
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

    // Clip-region waveforms (drawn over the grid, under the playhead).
    DrawWaveforms(lane);

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
    if (where.x < kTimelineHeaderWidth) {
        // Empty strip column below the last track: clear the selection.
        if (RowAtY(where.y) < 0)
            jackdaw_project_clear_selection(m_timeline->Project());
        return;
    }
    // Lane click: select the row's track (if any) and locate the transport.
    int row = RowAtY(where.y);
    if (row >= 0)
        jackdaw_project_select_single(m_timeline->Project(),
                                      jackdaw_project_get_track(m_timeline->Project(), row));
    m_dragging = true;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    m_timeline->LocateTo(m_timeline->XToFrame(where.x - kTimelineHeaderWidth));
}

void TrackAreaView::MouseUp(BPoint where)
{
    (void)where;
    m_dragging = false;
}

void TrackAreaView::MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage)
{
    (void)code;
    (void)dragMessage;
    if (m_dragging)
        m_timeline->LocateTo(m_timeline->XToFrame(where.x - kTimelineHeaderWidth));
}

void TrackAreaView::FrameResized(float w, float h)
{
    BView::FrameResized(w, h);
    m_timeline->UpdateVScrollBar();
    SetScrollY(m_scroll_y); // re-clamp against the new visible height
}

void TrackAreaView::MessageReceived(BMessage *message)
{
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
