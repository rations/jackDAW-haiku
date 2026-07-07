#include "FaderView.h"

#include <Message.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>

// dB taper: -42 .. +12 dB across the travel.
static const double kDbMin = -42.0;
static const double kDbMax = 12.0;
static const double kDbSpan = 54.0;

// Layout: a slim slider column on the left, the dB scale to its right.
static const float kFaderW = 20.0f;    // slider column width
static const float kScaleW = 24.0f;    // dB label column width
static const float kSliderHalf = 7.0f; // half the handle cap height
static const float kMinHeight = 96.0f;

static double pos_to_db(double p)
{
    if (p < 0.0)
        p = 0.0;
    if (p > 1.0)
        p = 1.0;
    return kDbMin + p * kDbSpan;
}

static double db_to_pos(double db)
{
    if (db < kDbMin)
        db = kDbMin;
    if (db > kDbMax)
        db = kDbMax;
    return (db - kDbMin) / kDbSpan;
}

FaderView::FaderView(const char *name, float gain, BMessage *message)
    : BView(name, B_WILL_DRAW | B_FRAME_EVENTS), BInvoker(message, NULL), m_pos(0.0),
      m_dragging(false)
{
    SetGain(gain);
}

void FaderView::AttachedToWindow()
{
    BView::AttachedToWindow();
    if (Parent())
        SetViewColor(Parent()->ViewColor());
    else
        SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    SetExplicitMinSize(BSize(kFaderW + kScaleW, kMinHeight));
    SetExplicitMaxSize(BSize(kFaderW + kScaleW, B_SIZE_UNLIMITED));
}

void FaderView::SetGain(float gain)
{
    double db = (gain > 0.0001f) ? 20.0 * log10((double)gain) : kDbMin;
    double pos = db_to_pos(db);
    if (pos == m_pos)
        return;
    m_pos = pos;
    if (Window())
        Invalidate();
}

double FaderView::YToPos(float y) const
{
    float usable = Bounds().Height() - 2.0f * kSliderHalf;
    if (usable < 1.0f)
        usable = Bounds().Height();
    double pos = 1.0 - (double)(y - kSliderHalf) / (double)usable;
    if (pos < 0.0)
        pos = 0.0;
    if (pos > 1.0)
        pos = 1.0;
    return pos;
}

float FaderView::PosToY(double pos) const
{
    float usable = Bounds().Height() - 2.0f * kSliderHalf;
    if (usable < 1.0f)
        usable = Bounds().Height();
    return kSliderHalf + (float)((1.0 - pos) * usable);
}

void FaderView::SetPosNotify(double pos)
{
    if (pos < 0.0)
        pos = 0.0;
    if (pos > 1.0)
        pos = 1.0;
    if (pos == m_pos)
        return;
    m_pos = pos;
    Invalidate();
    double db = pos_to_db(m_pos);
    float gain = (float)pow(10.0, db / 20.0);
    BMessage out(*Message());
    out.AddFloat("gain", gain);
    Invoke(&out);
}

void FaderView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect b = Bounds();

    // Slider trough (centred in the fader column).
    float tx = kFaderW / 2.0f;
    BRect trough(tx - 3.0f, kSliderHalf, tx + 3.0f, b.Height() - kSliderHalf);
    SetHighColor(30, 30, 33);
    FillRoundRect(trough, 3.0f, 3.0f);

    // Filled level from the handle down to the bottom.
    float hy = PosToY(m_pos);
    BRect fill(trough.left, hy, trough.right, trough.bottom);
    SetHighColor(70, 120, 90);
    FillRoundRect(fill, 3.0f, 3.0f);

    // Handle cap.
    BRect cap(2.0f, hy - kSliderHalf, kFaderW - 2.0f, hy + kSliderHalf);
    SetHighColor(200, 200, 206);
    FillRoundRect(cap, 2.0f, 2.0f);
    SetHighColor(120, 120, 126);
    StrokeRoundRect(cap, 2.0f, 2.0f);
    // Grip line across the cap.
    SetHighColor(90, 90, 96);
    StrokeLine(BPoint(cap.left + 2.0f, hy), BPoint(cap.right - 2.0f, hy));

    // dB scale to the right (labels every 6 dB, ticks next to the slider).
    SetFontSize(9.0f);
    font_height fh;
    GetFontHeight(&fh);
    rgb_color tick = {110, 110, 116, 255};
    rgb_color lbl = ui_color(B_PANEL_TEXT_COLOR);
    for (int d = (int)kDbMax; d >= (int)kDbMin; d -= 6) {
        float y = PosToY(db_to_pos((double)d));
        SetHighColor(tick);
        StrokeLine(BPoint(kFaderW, y), BPoint(kFaderW + 4.0f, y));
        char m[8];
        snprintf(m, sizeof(m), "%d", d);
        SetHighColor(lbl);
        DrawString(m, BPoint(kFaderW + 6.0f, y + (fh.ascent - fh.descent) / 2.0f));
    }
    SetFontSize(be_plain_font->Size());
}

void FaderView::MouseDown(BPoint where)
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
        SetPosNotify(db_to_pos(0.0)); // double-click -> unity
        return;
    }
    m_dragging = true;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    SetPosNotify(YToPos(where.y));
}

void FaderView::MouseUp(BPoint where)
{
    (void)where;
    m_dragging = false;
}

void FaderView::MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage)
{
    (void)code;
    (void)dragMessage;
    if (m_dragging)
        SetPosNotify(YToPos(where.y));
}

void FaderView::MessageReceived(BMessage *message)
{
    if (message->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            double step = 1.0 / kDbSpan; // ~1 dB per notch
            SetPosNotify(m_pos - (dy > 0 ? step : -step));
        }
        return;
    }
    BView::MessageReceived(message);
}
