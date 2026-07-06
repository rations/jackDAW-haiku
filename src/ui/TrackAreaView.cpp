#include "TrackAreaView.h"

#include <InterfaceDefs.h>
#include <Window.h>

#include "engine/jackdaw-engine.h"
#include "engine/tempomap.h"
#include "TimelineView.h"

// Empty-timeline palette (dark canvas like the Linux JackDAW wave area).
static const rgb_color kAreaBg = {45, 45, 48, 255};
static const rgb_color kHeaderBg = {58, 58, 62, 255};
static const rgb_color kBarLine = {82, 82, 88, 255};
static const rgb_color kBeatLine = {62, 62, 66, 255};
static const rgb_color kPlayhead = {230, 70, 60, 255};

TrackAreaView::TrackAreaView(TimelineView *timeline)
    : BView("track_area", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), m_timeline(timeline),
      m_dragging(false)
{
}

void TrackAreaView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect bounds = Bounds();

    // Strip column background (track strips land here in the next milestone).
    BRect header = bounds;
    header.right = kTimelineHeaderWidth - 1;
    SetHighColor(kHeaderBg);
    FillRect(header);

    BRect lane = bounds;
    lane.left = kTimelineHeaderWidth;
    SetHighColor(kAreaBg);
    FillRect(lane);

    // Beat/bar grid (when enabled).
    JackDawProject *p = m_timeline->Project();
    if (p->grid_enabled) {
        TempoMap tm;
        tempomap_from_project(&tm, p, jackdaw_engine_get_sample_rate());
        double fpbar = tempomap_frames_per_bar(&tm);
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
            (void)fpbar;
        }
    }

    // Playhead line.
    float x = lane.left + m_timeline->FrameToX(jackdaw_engine_get_play_pos());
    if (x >= lane.left && x <= lane.right) {
        SetHighColor(kPlayhead);
        StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
    }
}

void TrackAreaView::MouseDown(BPoint where)
{
    if (where.x < kTimelineHeaderWidth)
        return; /* strip column — nothing to do until strips exist */
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

void TrackAreaView::MessageReceived(BMessage *message)
{
    if (message->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0;
        if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0) {
            BPoint where;
            uint32 buttons;
            GetMouse(&where, &buttons, false);
            m_timeline->ZoomBy(dy > 0 ? 1.25 : 0.8, where.x - kTimelineHeaderWidth);
        }
        return;
    }
    BView::MessageReceived(message);
}
