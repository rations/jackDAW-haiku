#pragma once

#include <View.h>

#include "engine/project.h"

class BScrollBar;
class RulerView;
class TrackAreaView;

// Timeline geometry (pixel metrics mirror the Linux JackDAW timeline).
static const float kTimelineHeaderWidth = 235.0f; // track strip column
static const float kTimelineRulerHeight = 28.0f;
static const float kTimelineTrackHeight = 100.0f; // room for the strip's name/buttons/knobs/input

// TimelineView: the ruler + track area + horizontal scrollbar, and the owner
// of the horizontal view model every child shares:
//   view_start = timeline frame at the left edge of the lane area
//   spp        = samples per pixel (zoom)
// Scrolling is virtual — children draw from (view_start, spp); the scrollbar
// works in pixels at the current zoom, so BView/float coordinates stay small
// even for hour-long projects. All methods run on the window's looper thread.
class TimelineView : public BView
{
public:
    explicit TimelineView(JackDawProject *project);

    void AttachedToWindow() override;
    void DetachedFromWindow() override;

    JackDawProject *Project() const
    {
        return m_project;
    }

    // ---- Horizontal view model ----
    off_t ViewStart() const
    {
        return m_view_start;
    }
    double Spp() const
    {
        return m_spp;
    }
    // x is in lane coordinates (0 = left edge of the lane area).
    float FrameToX(off_t frame) const
    {
        return (float)((double)(frame - m_view_start) / m_spp);
    }
    off_t XToFrame(float x) const;
    float LaneWidth() const;

    void SetViewStart(off_t frame);
    // Zoom by `factor` (>1 zooms out), keeping lane x `center_x` stationary.
    void ZoomBy(double factor, float center_x);

    // Locate the transport to `frame`, snapped when snap is enabled.
    void LocateTo(off_t frame);

    // Called by the window's UI tick: playhead strip invalidation +
    // follow-scroll while playing.
    void TickUpdate();

    // Full redraw of ruler + lanes (timing/grid/zoom changes).
    void InvalidateAll();

    // Vertical (track) scrolling.
    void UpdateVScrollBar();       // re-derive range from track content height
    void ScrollTracksBy(float dy); // wheel scroll the track column

    void FrameResized(float w, float h) override;

private:
    void UpdateScrollBar();
    friend class TimelineScrollBar;
    friend class TimelineVScrollBar;

    JackDawProject *m_project; // borrowed

    RulerView *m_ruler;
    TrackAreaView *m_track_area;
    BScrollBar *m_hscroll;
    BScrollBar *m_vscroll;

    off_t m_view_start;
    double m_spp;
    off_t m_last_playhead;
    gulong m_timing_handler; // GObject signal handler id (0 = none)
};
