#include "TimelineView.h"

#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <ScrollBar.h>
#include <SpaceLayoutItem.h>

#include "engine/jackdaw-engine.h"
#include "RulerView.h"
#include "TrackAreaView.h"

// Zoom limits (samples per pixel) and defaults.
static const double kMinSpp = 8.0;
static const double kMaxSpp = 262144.0;
static const double kDefaultSpp = 1000.0; // ~17 s visible in an 800 px lane at 48 kHz

// The horizontal scrollbar works in pixels at the current zoom; its range is
// re-derived from content length whenever zoom/position change.
class TimelineScrollBar : public BScrollBar
{
public:
    explicit TimelineScrollBar(TimelineView *timeline)
        : BScrollBar("timeline_hscroll", NULL, 0, 0, B_HORIZONTAL), m_timeline(timeline)
    {
    }

    void ValueChanged(float newValue) override
    {
        BScrollBar::ValueChanged(newValue);
        m_timeline->SetViewStart((off_t)((double)newValue * m_timeline->Spp()));
    }

private:
    TimelineView *m_timeline;
};

// Vertical scrollbar for the track column: value is a pixel offset into the
// stacked track rows.
class TimelineVScrollBar : public BScrollBar
{
public:
    explicit TimelineVScrollBar(TimelineView *timeline)
        : BScrollBar("timeline_vscroll", NULL, 0, 0, B_VERTICAL), m_timeline(timeline)
    {
    }

    void ValueChanged(float newValue) override
    {
        BScrollBar::ValueChanged(newValue);
        m_timeline->m_track_area->SetScrollY(newValue);
    }

private:
    TimelineView *m_timeline;
};

// GObject "timing-changed" -> full timeline redraw. Emitted only from this
// window's looper thread (all mutating project calls happen there), so the
// direct Invalidate is thread-safe.
static void timeline_timing_changed(JackDawProject *project, gpointer user)
{
    (void)project;
    static_cast<TimelineView *>(user)->InvalidateAll();
}

TimelineView::TimelineView(JackDawProject *project)
    : BView("timeline", B_WILL_DRAW | B_FRAME_EVENTS), m_project(project), m_ruler(NULL),
      m_track_area(NULL), m_hscroll(NULL), m_vscroll(NULL), m_view_start(0), m_spp(kDefaultSpp),
      m_last_playhead(-1), m_timing_handler(0)
{
    m_ruler = new RulerView(this);
    m_track_area = new TrackAreaView(this);
    m_hscroll = new TimelineScrollBar(this);
    m_vscroll = new TimelineVScrollBar(this);

    // Frozen-pane layout: fixed strip column (left) + ruler (top), the track
    // area scrolling both ways. The corner struts keep the columns aligned with
    // the vertical scrollbar gutter.
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 0)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(kTimelineHeaderWidth))
        .Add(m_ruler)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(B_V_SCROLL_BAR_WIDTH))
        .End()
        .AddGroup(B_HORIZONTAL, 0)
        .Add(m_track_area)
        .Add(m_vscroll)
        .End()
        .AddGroup(B_HORIZONTAL, 0)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(kTimelineHeaderWidth))
        .Add(m_hscroll)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(B_V_SCROLL_BAR_WIDTH))
        .End();
}

void TimelineView::AttachedToWindow()
{
    BView::AttachedToWindow();
    m_timing_handler =
        g_signal_connect(m_project, "timing-changed", G_CALLBACK(timeline_timing_changed), this);
    UpdateScrollBar();
}

void TimelineView::DetachedFromWindow()
{
    if (m_timing_handler != 0) {
        g_signal_handler_disconnect(m_project, m_timing_handler);
        m_timing_handler = 0;
    }
    BView::DetachedFromWindow();
}

off_t TimelineView::XToFrame(float x) const
{
    double frame = (double)m_view_start + (double)x * m_spp;
    return frame < 0.0 ? 0 : (off_t)frame;
}

float TimelineView::LaneWidth() const
{
    float w = Bounds().Width() - kTimelineHeaderWidth;
    return w > 1.0f ? w : 1.0f;
}

void TimelineView::SetViewStart(off_t frame)
{
    if (frame < 0)
        frame = 0;
    if (frame == m_view_start)
        return;
    m_view_start = frame;
    UpdateScrollBar();
    InvalidateAll();
}

void TimelineView::ZoomBy(double factor, float center_x)
{
    double new_spp = m_spp * factor;
    if (new_spp < kMinSpp)
        new_spp = kMinSpp;
    if (new_spp > kMaxSpp)
        new_spp = kMaxSpp;
    if (new_spp == m_spp)
        return;
    if (center_x < 0.0f)
        center_x = 0.0f;

    // Keep the frame under center_x stationary on screen.
    off_t anchor = XToFrame(center_x);
    double start = (double)anchor - (double)center_x * new_spp;
    m_spp = new_spp;
    m_view_start = start < 0.0 ? 0 : (off_t)start;
    UpdateScrollBar();
    InvalidateAll();
}

void TimelineView::LocateTo(off_t frame)
{
    frame = jackdaw_project_snap_frame(m_project, frame, jackdaw_engine_get_sample_rate());
    jackdaw_engine_locate(frame);
    // Repaint immediately rather than waiting for the next tick.
    InvalidateAll();
}

void TimelineView::TickUpdate()
{
    m_track_area->UpdateMeters();

    off_t pos = jackdaw_engine_get_play_pos();
    if (pos == m_last_playhead)
        return;

    float lane_w = LaneWidth();
    float x_new = FrameToX(pos);

    // Follow-scroll: page the view once the playhead nears the right edge.
    if (jackdaw_engine_is_playing() && x_new > lane_w * 0.95f) {
        m_last_playhead = pos;
        SetViewStart((off_t)((double)pos - 0.05 * (double)lane_w * m_spp));
        return;
    }

    // Otherwise repaint just the old and new playhead strips.
    float x_old = FrameToX(m_last_playhead);
    m_last_playhead = pos;
    BRect r = m_ruler->Bounds();
    m_ruler->Invalidate(BRect(x_old - 5, r.top, x_old + 5, r.bottom));
    m_ruler->Invalidate(BRect(x_new - 5, r.top, x_new + 5, r.bottom));
    BRect a = m_track_area->Bounds();
    // While recording, the live waveform grows in the band the playhead sweeps,
    // so invalidate the whole span between the old and new position; otherwise
    // just the two narrow playhead strips.
    if (jackdaw_engine_is_recording()) {
        float lo = (x_old < x_new ? x_old : x_new) - 2;
        float hi = (x_old > x_new ? x_old : x_new) + 2;
        m_track_area->Invalidate(
            BRect(kTimelineHeaderWidth + lo, a.top, kTimelineHeaderWidth + hi, a.bottom));
    } else {
        m_track_area->Invalidate(BRect(kTimelineHeaderWidth + x_old - 2, a.top,
                                       kTimelineHeaderWidth + x_old + 2, a.bottom));
        m_track_area->Invalidate(BRect(kTimelineHeaderWidth + x_new - 2, a.top,
                                       kTimelineHeaderWidth + x_new + 2, a.bottom));
    }
}

void TimelineView::InvalidateAll()
{
    m_ruler->Invalidate();
    m_track_area->Invalidate();
}

void TimelineView::UpdateScrollBar()
{
    // Content = whatever is furthest: playhead + slack, the visible span, or
    // a 5-minute floor. Track/region extents join this max in later phases.
    guint32 sr = jackdaw_engine_get_sample_rate();
    double content = (double)jackdaw_engine_get_play_pos() + 30.0 * sr;
    double floor_frames = 300.0 * sr;
    if (content < floor_frames)
        content = floor_frames;
    double visible_end = (double)m_view_start + (double)LaneWidth() * m_spp;
    if (content < visible_end)
        content = visible_end;

    float max_px = (float)(content / m_spp - (double)LaneWidth());
    if (max_px < 0)
        max_px = 0;
    m_hscroll->SetRange(0, max_px);
    m_hscroll->SetValue((float)((double)m_view_start / m_spp));
    m_hscroll->SetProportion((float)((double)LaneWidth() / (content / m_spp)));
    m_hscroll->SetSteps(LaneWidth() * 0.1f, LaneWidth() * 0.9f);
}

void TimelineView::UpdateVScrollBar()
{
    if (!m_vscroll || !m_track_area)
        return;
    float content = m_track_area->ContentHeight();
    float visible = m_track_area->Bounds().Height();
    if (visible < 1.0f)
        visible = 1.0f;
    float maxv = content - visible;
    if (maxv < 0.0f)
        maxv = 0.0f;
    m_vscroll->SetRange(0, maxv);
    m_vscroll->SetProportion(content > 0.0f ? visible / content : 1.0f);
    m_vscroll->SetSteps(kTimelineTrackHeight, visible);
    if (m_vscroll->Value() > maxv)
        m_vscroll->SetValue(maxv);
}

void TimelineView::ScrollTracksBy(float dy)
{
    if (m_vscroll)
        m_vscroll->SetValue(m_vscroll->Value() + dy); // clamped to range; drives SetScrollY
}

void TimelineView::FrameResized(float w, float h)
{
    BView::FrameResized(w, h);
    UpdateScrollBar();
    UpdateVScrollBar();
}
