#include "StateButton.h"

#include <ControlLook.h>
#include <math.h>

StateButton::StateButton(const char *name, const char *label, BMessage *message)
    : BButton(name, label, message), m_active_base((rgb_color){60, 150, 230, 255}),
      m_active_text((rgb_color){255, 255, 255, 255}), m_has_active_color(false),
      m_forced_active(false), m_ignore_value(false), m_glyph(GLYPH_LABEL), m_rec_recording(false),
      m_rec_punch(false)
{
}

void StateButton::SetActiveColor(rgb_color base, rgb_color text)
{
    m_active_base = base;
    m_active_text = text;
    m_has_active_color = true;
    Invalidate();
}

void StateButton::SetForcedActive(bool active)
{
    if (m_forced_active == active)
        return;
    m_forced_active = active;
    Invalidate();
}

void StateButton::SetActiveIgnoresValue(bool ignore)
{
    m_ignore_value = ignore;
    Invalidate();
}

void StateButton::SetGlyph(GlyphKind kind)
{
    m_glyph = kind;
    Invalidate();
}

void StateButton::SetRecordState(bool recording, bool punch)
{
    if (m_rec_recording == recording && m_rec_punch == punch)
        return;
    m_rec_recording = recording;
    m_rec_punch = punch;
    Invalidate();
}

bool StateButton::IsActiveState() const
{
    if (m_forced_active)
        return true;
    if (m_ignore_value)
        return false;
    return Value() == B_CONTROL_ON;
}

// Faithful copy of BButton::Draw (native frame + background + label via
// be_control_look), substituting the base/text colours while active.
void StateButton::Draw(BRect updateRect)
{
    BRect rect(Bounds());
    rgb_color background = ViewColor();

    const bool active = IsActiveState() && m_has_active_color;
    rgb_color base = active ? m_active_base : ui_color(B_CONTROL_BACKGROUND_COLOR);
    rgb_color text = active ? m_active_text : ui_color(B_CONTROL_TEXT_COLOR);

    uint32 flags = be_control_look->Flags(this);

    be_control_look->DrawButtonFrame(this, rect, updateRect, base, background, flags);
    be_control_look->DrawButtonBackground(this, rect, updateRect, base, flags);

    switch (m_glyph) {
        case GLYPH_LOOP:
            DrawLoopGlyph(rect, text);
            break;
        case GLYPH_RECORD:
            DrawRecordGlyph(rect);
            break;
        case GLYPH_LABEL:
        default:
            be_control_look->DrawLabel(this, Label(), rect, updateRect, base, flags,
                                       BAlignment(B_ALIGN_CENTER, B_ALIGN_MIDDLE), &text);
            break;
    }
}

// A circular arrow: a stroked ring with a gap, plus a filled arrowhead at the
// gap. Drawn as a vector so it never depends on a font carrying U+27F3.
void StateButton::DrawLoopGlyph(BRect rect, rgb_color color)
{
    BPoint c(rect.left + rect.Width() / 2.0f, rect.top + rect.Height() / 2.0f);
    float side = rect.Width() < rect.Height() ? rect.Width() : rect.Height();
    float r = side / 2.0f - 5.0f;
    if (r < 3.0f)
        r = 3.0f;

    SetHighColor(color);
    SetPenSize(2.0f);
    SetDrawingMode(B_OP_OVER);

    // Screen coordinates (y down): point(t) = (cx + r*cos t, cy + r*sin t), the
    // same convention KnobView uses. Increasing t runs clockwise on screen.
    // Draw the ring as a short polyline from -60deg round to 240deg, leaving a
    // ~60deg gap at the top for the arrowhead.
    const float d2r = (float)M_PI / 180.0f;
    const float a_start = -60.0f * d2r;
    const float a_end = 240.0f * d2r;
    const int steps = 40;
    BPoint prev(c.x + cosf(a_start) * r, c.y + sinf(a_start) * r);
    for (int i = 1; i <= steps; i++) {
        float t = a_start + (a_end - a_start) * (float)i / (float)steps;
        BPoint p(c.x + cosf(t) * r, c.y + sinf(t) * r);
        StrokeLine(prev, p);
        prev = p;
    }

    // Filled arrowhead at the arc's end, tip pointing along the (clockwise)
    // tangent so it reads as a circular arrow curling up into the gap.
    BPoint p(c.x + cosf(a_end) * r, c.y + sinf(a_end) * r);
    float tx = -sinf(a_end), ty = cosf(a_end); // unit tangent (increasing t)
    float nx = ty, ny = -tx;                   // unit normal
    float hl = r * 0.9f, hw = r * 0.6f;
    BPoint tip(p.x + tx * hl * 0.5f, p.y + ty * hl * 0.5f);
    BPoint back(p.x - tx * hl * 0.5f, p.y - ty * hl * 0.5f);
    BPoint b1(back.x + nx * hw, back.y + ny * hw);
    BPoint b2(back.x - nx * hw, back.y - ny * hw);
    FillTriangle(tip, b1, b2);

    SetPenSize(1.0f);
}

// Hollow ring in normal mode; ring + filled centre dot (bullseye) in punch
// mode. Glyph is white while actively recording (over the red active base),
// otherwise red on the resting button.
void StateButton::DrawRecordGlyph(BRect rect)
{
    BPoint c(rect.left + rect.Width() / 2.0f, rect.top + rect.Height() / 2.0f);
    float side = rect.Width() < rect.Height() ? rect.Width() : rect.Height();
    float r = side / 2.0f - 5.0f;
    if (r < 3.0f)
        r = 3.0f;

    rgb_color color =
        m_rec_recording ? (rgb_color){255, 255, 255, 255} : (rgb_color){204, 51, 43, 255};
    SetHighColor(color);
    SetDrawingMode(B_OP_OVER);

    SetPenSize(2.0f);
    StrokeEllipse(c, r, r);
    if (m_rec_punch)
        FillEllipse(c, r * 0.42f, r * 0.42f);
    SetPenSize(1.0f);
}
