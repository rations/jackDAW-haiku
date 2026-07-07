#include "VuView.h"

#include <math.h>

// dBFS range shown (matches the Linux JackDAW meter: -60 .. +6, span 66).
static const double kDbFloor = -60.0;
static const double kDbCeil = 6.0;
static const double kDbSpan = kDbCeil - kDbFloor;

VuView::VuView(const char *name, float min_width)
    : BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE), m_left(0.0f), m_right(0.0f),
      m_min_width(min_width)
{
}

void VuView::AttachedToWindow()
{
    BView::AttachedToWindow();
    if (Parent())
        SetViewColor(Parent()->ViewColor());
    else
        SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    SetExplicitMinSize(BSize(m_min_width, 40.0f));
    SetExplicitMaxSize(BSize(m_min_width, B_SIZE_UNLIMITED));
}

void VuView::SetPeaks(float left, float right)
{
    // UI-side decay hold so the fall-off is smooth.
    m_left = (left > m_left) ? left : m_left * 0.80f;
    m_right = (right > m_right) ? right : m_right * 0.80f;
    if (m_left < 0.00001f)
        m_left = 0.0f;
    if (m_right < 0.00001f)
        m_right = 0.0f;
    if (Window())
        Invalidate();
}

void VuView::DrawBar(BRect r, float disp)
{
    // Unlit bar background.
    SetHighColor(46, 46, 46);
    FillRect(r);

    if (disp <= 0.0001f)
        return;
    double db = 20.0 * log10((double)disp);
    double dbc = db;
    if (dbc < kDbFloor)
        dbc = kDbFloor;
    if (dbc > kDbCeil)
        dbc = kDbCeil;
    double frac = (dbc - kDbFloor) / kDbSpan;
    float h = r.Height();
    float fh = (float)(frac * h);
    if (fh <= 0.0f)
        return;

    // Single solid fill from the bottom, coloured by the peak level (exactly the
    // Linux meter: red at/over 0 dBFS, yellow above -12, green below).
    if (db >= 0.0)
        SetHighColor(230, 38, 38);
    else if (db >= -12.0)
        SetHighColor(217, 199, 26);
    else
        SetHighColor(38, 174, 51);
    FillRect(BRect(r.left, r.bottom - fh, r.right, r.bottom));
}

void VuView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect b = Bounds();
    float mid = b.Width() / 2.0f;
    BRect left(b.left + 1.0f, b.top + 1.0f, mid - 1.0f, b.bottom - 1.0f);
    BRect right(mid + 1.0f, b.top + 1.0f, b.right - 1.0f, b.bottom - 1.0f);
    DrawBar(left, m_left);
    DrawBar(right, m_right);
}
