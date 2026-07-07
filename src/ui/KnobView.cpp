#include "KnobView.h"

#include <ControlLook.h>
#include <Message.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// Geometry (mirrors the Linux knob: 28 px dial + a 12 px readout strip below).
static const float kKnobDiam = 28.0f;
static const float kKnobTextH = 12.0f;
// Sweep, in radians, in screen coordinates (y down) — identical math to the
// Linux Cairo version: min at ~7:30, max at ~4:30, 270° clockwise.
static const double kStartAng = -M_PI * 5.0 / 4.0;
static const double kEndAng = M_PI / 4.0;

KnobView::KnobView(const char *name, double min_value, double max_value, double value,
                   double default_value, Kind kind, BMessage *message)
    : BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), BInvoker(message, NULL), m_value(value),
      m_min(min_value), m_max(max_value), m_default(default_value), m_kind(kind), m_dragging(false),
      m_drag_start_y(0.0f), m_drag_start_val(0.0)
{
    m_center[0] = '\0';
    if (m_value < m_min)
        m_value = m_min;
    if (m_value > m_max)
        m_value = m_max;
}

void KnobView::AttachedToWindow()
{
    BView::AttachedToWindow();
    // Own an opaque background so app_server erases the view before each Draw;
    // inheriting a parent that draws its own (transparent) background would leave
    // the moving pointer dot as a smear.
    rgb_color bg = ui_color(B_PANEL_BACKGROUND_COLOR);
    if (Parent()) {
        rgb_color pc = Parent()->ViewColor();
        if (pc.alpha != 0) // not B_TRANSPARENT_COLOR
            bg = pc;
    }
    SetViewColor(bg);
    SetLowColor(bg);
    float w = kKnobDiam + 8.0f;
    SetExplicitMinSize(BSize(w, kKnobDiam + kKnobTextH));
    SetExplicitMaxSize(BSize(w, kKnobDiam + kKnobTextH));
}

void KnobView::SetValue(double value)
{
    if (value < m_min)
        value = m_min;
    if (value > m_max)
        value = m_max;
    if (value == m_value)
        return;
    m_value = value;
    if (Window())
        Invalidate();
}

void KnobView::SetCenterLabel(const char *label)
{
    if (label)
        strlcpy(m_center, label, sizeof(m_center));
    else
        m_center[0] = '\0';
    if (Window())
        Invalidate();
}

void KnobView::FormatValue(char *buf, size_t len) const
{
    switch (m_kind) {
        case KIND_DB:
            if (m_value <= m_min + 0.01)
                snprintf(buf, len, "-inf");
            else
                snprintf(buf, len, "%+.1f", m_value);
            break;
        case KIND_PAN: {
            int p = (int)lround(fabs(m_value) * 100.0);
            if (p == 0)
                snprintf(buf, len, "C");
            else if (m_value < 0)
                snprintf(buf, len, "L%d", p);
            else
                snprintf(buf, len, "R%d", p);
            break;
        }
        default:
            snprintf(buf, len, "%.2f", m_value);
            break;
    }
}

void KnobView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect b = Bounds();
    // Clear to the (opaque) background first — belt to app_server's erase, so the
    // moving pointer never leaves a trail.
    SetHighColor(LowColor());
    FillRect(b);
    float cx = b.Width() / 2.0f;
    float cy = kKnobDiam / 2.0f + 1.0f;
    float r = kKnobDiam / 2.0f - 2.5f;

    double t = (m_max > m_min) ? (m_value - m_min) / (m_max - m_min) : 0.0;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    double ang = kStartAng + t * (kEndAng - kStartAng);

    // Dial face.
    SetHighColor(60, 60, 64);
    FillEllipse(BPoint(cx, cy), r, r);
    SetHighColor(90, 90, 96);
    SetPenSize(1.0f);
    StrokeEllipse(BPoint(cx, cy), r, r);

    // Value pointer (bright line from centre to rim + a dot at the tip).
    float px = cx + (float)((r - 1.5) * cos(ang));
    float py = cy + (float)((r - 1.5) * sin(ang));
    SetHighColor(60, 150, 230);
    SetPenSize(2.0f);
    StrokeLine(BPoint(cx, cy), BPoint(px, py));
    SetHighColor(240, 240, 240);
    FillEllipse(BPoint(px, py), 2.0f, 2.0f);

    // Centre identifier letter.
    if (m_center[0]) {
        SetHighColor(180, 180, 180);
        SetFontSize(9.0f);
        float tw = StringWidth(m_center);
        font_height fh;
        GetFontHeight(&fh);
        DrawString(m_center, BPoint(cx - tw / 2.0f, cy + (fh.ascent - fh.descent) / 2.0f));
    }

    // Readout strip below the dial.
    char buf[24];
    FormatValue(buf, sizeof(buf));
    SetFontSize(9.0f);
    float tw = StringWidth(buf);
    rgb_color lc = ui_color(B_PANEL_TEXT_COLOR);
    SetHighColor(lc);
    DrawString(buf, BPoint(cx - tw / 2.0f, kKnobDiam + kKnobTextH - 2.0f));
    SetFontSize(be_plain_font->Size());
}

void KnobView::ApplyDelta(double new_value)
{
    if (new_value < m_min)
        new_value = m_min;
    if (new_value > m_max)
        new_value = m_max;
    if (new_value == m_value)
        return;
    m_value = new_value;
    Invalidate();
    BMessage out(*Message());
    out.AddFloat("value", (float)m_value);
    Invoke(&out);
}

void KnobView::MouseDown(BPoint where)
{
    int32 buttons = 0;
    int32 clicks = 0;
    BMessage *msg = Window() ? Window()->CurrentMessage() : NULL;
    if (msg) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("clicks", &clicks);
    }
    if (buttons & (B_SECONDARY_MOUSE_BUTTON | B_TERTIARY_MOUSE_BUTTON))
        return;

    if (clicks >= 2) {
        m_dragging = false;
        ApplyDelta(m_default); // double-click resets
        return;
    }
    m_dragging = true;
    m_drag_start_y = where.y;
    m_drag_start_val = m_value;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}

void KnobView::MouseUp(BPoint where)
{
    (void)where;
    m_dragging = false;
}

void KnobView::MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage)
{
    (void)code;
    (void)dragMessage;
    if (!m_dragging)
        return;
    double range = m_max - m_min;
    double delta = (double)(m_drag_start_y - where.y) * range / 150.0;
    ApplyDelta(m_drag_start_val + delta);
}

void KnobView::MessageReceived(BMessage *message)
{
    if (message->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            double step = (m_max - m_min) / 200.0;
            ApplyDelta(m_value - (dy > 0 ? step : -step) * fabs(dy));
        }
        return;
    }
    BView::MessageReceived(message);
}
