#include "RulerView.h"

#include <Font.h>
#include <InterfaceDefs.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/jackdaw-engine.h"
#include "engine/tempomap.h"
#include "engine/timeruler.h"
#include "TimelineView.h"

// Tick geometry
static const int kMaxTicks = 256;
static const float kMajorTickLen = 12.0f;
static const float kMidTickLen = 8.0f;
static const float kMinorTickLen = 4.0f;

RulerView::RulerView(TimelineView *timeline)
    : BView("ruler", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), m_timeline(timeline),
      m_dragging(false), m_loop_drag_edge(0)
{
    SetExplicitMinSize(BSize(B_SIZE_UNSET, kTimelineRulerHeight));
    SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, kTimelineRulerHeight));
}

void RulerView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect bounds = Bounds();

    rgb_color base = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_1_TINT);
    SetHighColor(base);
    FillRect(bounds);
    SetHighColor(tint_color(base, B_DARKEN_2_TINT));
    StrokeLine(BPoint(bounds.left, bounds.bottom), BPoint(bounds.right, bounds.bottom));

    BFont font(be_plain_font);
    font.SetSize(font.Size() * 0.85f);
    SetFont(&font);

    if (m_timeline->Project()->ruler_mode == JACKDAW_RULER_BARS)
        DrawBarTicks(bounds);
    else
        DrawTimeTicks(bounds);

    DrawLoop(bounds);

    // Playhead marker: a line with a small triangle hanging from the top.
    float x = m_timeline->FrameToX(jackdaw_engine_get_play_pos());
    if (x >= bounds.left - 4 && x <= bounds.right + 4) {
        SetHighColor(230, 70, 60);
        StrokeLine(BPoint(x, bounds.top), BPoint(x, bounds.bottom));
        FillTriangle(BPoint(x - 4, bounds.top), BPoint(x + 4, bounds.top),
                     BPoint(x, bounds.top + 6));
    }
}

void RulerView::DrawTimeTicks(BRect bounds)
{
    guint32 sr = jackdaw_engine_get_sample_rate();
    off_t start = m_timeline->ViewStart();
    off_t end = m_timeline->XToFrame(bounds.right);

    off_t major[kMaxTicks], mid[kMaxTicks], minor[kMaxTicks];
    // Budgets follow label/tick spacing: labels every ~80 px, ticks every ~8.
    int nmajor = (int)(bounds.Width() / 80.0f) + 2;
    int nmid = (int)(bounds.Width() / 24.0f) + 2;
    int nminor = (int)(bounds.Width() / 8.0f) + 2;
    if (nmajor > kMaxTicks)
        nmajor = kMaxTicks;
    if (nmid > kMaxTicks)
        nmid = kMaxTicks;
    if (nminor > kMaxTicks)
        nminor = kMaxTicks;

    ruler_tick_positions(sr, start, end, major, &nmajor, mid, &nmid, minor, &nminor, TIMEMODE_REAL);

    rgb_color fg = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_4_TINT);
    rgb_color dim = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_3_TINT);

    SetHighColor(dim);
    for (int i = 0; i < nminor; i++) {
        float x = m_timeline->FrameToX(minor[i]);
        StrokeLine(BPoint(x, bounds.bottom - kMinorTickLen), BPoint(x, bounds.bottom));
    }
    for (int i = 0; i < nmid; i++) {
        float x = m_timeline->FrameToX(mid[i]);
        StrokeLine(BPoint(x, bounds.bottom - kMidTickLen), BPoint(x, bounds.bottom));
    }

    SetHighColor(fg);
    font_height fh;
    GetFontHeight(&fh);
    for (int i = 0; i < nmajor; i++) {
        float x = m_timeline->FrameToX(major[i]);
        StrokeLine(BPoint(x, bounds.bottom - kMajorTickLen), BPoint(x, bounds.bottom));
        char buf[64];
        format_timecode(sr, major[i], 0, buf, TIMEMODE_REAL);
        DrawString(buf, BPoint(x + 3, bounds.bottom - kMajorTickLen + 1));
    }
}

void RulerView::DrawBarTicks(BRect bounds)
{
    TempoMap tm;
    tempomap_from_project(&tm, m_timeline->Project(), jackdaw_engine_get_sample_rate());
    double fpbar = tempomap_frames_per_bar(&tm);
    double fpbeat = tempomap_frames_per_beat(&tm);
    double spp = m_timeline->Spp();
    if (fpbar <= 0.0 || spp <= 0.0)
        return;

    // Label a bar every 1/2/4/8/16… bars so labels stay >= ~70 px apart.
    double bar_px = fpbar / spp;
    int bar_step = 1;
    while (bar_px * bar_step < 70.0)
        bar_step *= 2;

    off_t start = m_timeline->ViewStart();
    off_t end = m_timeline->XToFrame(bounds.right);

    rgb_color fg = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_4_TINT);
    rgb_color dim = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_3_TINT);
    font_height fh;
    GetFontHeight(&fh);

    // Beat ticks (only when they have room).
    if (fpbeat / spp >= 7.0) {
        SetHighColor(dim);
        gint64 first_beat = (gint64)((double)start / fpbeat);
        for (gint64 b = first_beat;; b++) {
            off_t frame = (off_t)((double)b * fpbeat + 0.5);
            if (frame > end)
                break;
            if (frame < start)
                continue;
            float x = m_timeline->FrameToX(frame);
            StrokeLine(BPoint(x, bounds.bottom - kMinorTickLen), BPoint(x, bounds.bottom));
        }
    }

    // Bar ticks: every bar gets a mid tick; every bar_step-th a label.
    gint64 first_bar = (gint64)((double)start / fpbar);
    for (gint64 bar = first_bar;; bar++) {
        off_t frame = (off_t)((double)bar * fpbar + 0.5);
        if (frame > end)
            break;
        if (frame < start)
            continue;
        float x = m_timeline->FrameToX(frame);
        if (bar % bar_step == 0) {
            SetHighColor(fg);
            StrokeLine(BPoint(x, bounds.bottom - kMajorTickLen), BPoint(x, bounds.bottom));
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)(bar + 1));
            DrawString(buf, BPoint(x + 3, bounds.bottom - kMajorTickLen + 1));
        } else {
            SetHighColor(dim);
            StrokeLine(BPoint(x, bounds.bottom - kMidTickLen), BPoint(x, bounds.bottom));
        }
    }
}

// Amber loop-region band + two draggable end tabs, drawn over the ticks in
// either ruler mode. The tabs are always meaningful: with no region set both
// edges sit at frame 0, so a drag from there creates one. The band is brighter
// while looping is enabled.
void RulerView::DrawLoop(BRect bounds)
{
    if (m_timeline->Spp() <= 0.0)
        return;

    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    bool has = jackdaw_engine_has_loop_region();
    bool on = jackdaw_engine_get_loop_enabled();

    float x0 = m_timeline->FrameToX(ls);
    float x1 = m_timeline->FrameToX(le);

    if (has && x1 >= bounds.left && x0 <= bounds.right) {
        float bx0 = x0 < bounds.left ? bounds.left : x0;
        float bx1 = x1 > bounds.right ? bounds.right : x1;
        rgb_color band = {243, 166, 26, (uint8)(on ? 90 : 45)};
        SetDrawingMode(B_OP_ALPHA);
        SetHighColor(band);
        FillRect(BRect(bx0, bounds.top, bx1, bounds.bottom));
        SetDrawingMode(B_OP_COPY);
    }

    rgb_color tab = {243, 166, 26, 255};
    SetHighColor(tab);
    if (x0 >= bounds.left - 5 && x0 <= bounds.right + 5)
        FillRect(BRect(x0, bounds.top, x0 + 4, bounds.bottom)); // start tab: right of x0
    if (x1 >= bounds.left - 5 && x1 <= bounds.right + 5)
        FillRect(BRect(x1 - 4, bounds.top, x1, bounds.bottom)); // end tab: left of x1
}

int RulerView::LoopHit(float x) const
{
    if (m_timeline->Spp() <= 0.0)
        return 0;
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    float x0 = m_timeline->FrameToX(ls);
    float x1 = m_timeline->FrameToX(le);
    if (!jackdaw_engine_has_loop_region())
        return (fabsf(x - x0) <= 6.0f) ? 2 : 0; // no region: grab end tab, drag right
    if (fabsf(x - x0) <= 6.0f)
        return 1;
    if (fabsf(x - x1) <= 6.0f)
        return 2;
    return 0;
}

void RulerView::LoopDragTo(float x)
{
    off_t frame = m_timeline->XToFrame(x);
    if (frame < 0)
        frame = 0;
    frame =
        jackdaw_project_snap_frame(m_timeline->Project(), frame, jackdaw_engine_get_sample_rate());

    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    if (m_loop_drag_edge == 1) { // start tab
        if (frame > le)
            frame = le;
        jackdaw_engine_set_loop_range(frame, le);
    } else { // end tab
        if (frame < ls)
            frame = ls;
        jackdaw_engine_set_loop_range(ls, frame);
    }
    m_timeline->InvalidateAll();
}

void RulerView::MouseDown(BPoint where)
{
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);

    // A loop tab takes priority over scrubbing.
    int edge = LoopHit(where.x);
    if (edge) {
        m_loop_drag_edge = edge;
        LoopDragTo(where.x);
        return;
    }

    m_dragging = true;
    m_timeline->LocateTo(m_timeline->XToFrame(where.x));
}

void RulerView::MouseUp(BPoint where)
{
    (void)where;
    m_dragging = false;
    m_loop_drag_edge = 0;
}

void RulerView::MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage)
{
    (void)code;
    (void)dragMessage;
    if (m_loop_drag_edge)
        LoopDragTo(where.x);
    else if (m_dragging)
        m_timeline->LocateTo(m_timeline->XToFrame(where.x));
}

void RulerView::MessageReceived(BMessage *message)
{
    if (message->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0;
        if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0) {
            BPoint where;
            uint32 buttons;
            GetMouse(&where, &buttons, false);
            m_timeline->ZoomBy(dy > 0 ? 1.25 : 0.8, where.x);
        }
        return;
    }
    BView::MessageReceived(message);
}
