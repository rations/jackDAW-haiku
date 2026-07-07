#include "MixerView.h"

#include <ScrollBar.h>
#include <Window.h>

#include <algorithm>

#include "engine/jackdaw-engine.h"
#include "MixerStripView.h"

// Every strip is this wide; the row scrolls when the strips overflow the view.
static const float kStripW = kMixerStripW;

// Horizontal scrollbar that drives the mixer's strip offset.
class MixerHScrollBar : public BScrollBar
{
public:
    explicit MixerHScrollBar(MixerView *mixer)
        : BScrollBar("mixer_hscroll", NULL, 0, 0, B_HORIZONTAL), m_mixer(mixer)
    {
    }

    void ValueChanged(float newValue) override
    {
        BScrollBar::ValueChanged(newValue);
        m_mixer->SetScrollX(newValue);
    }

private:
    MixerView *m_mixer;
};

MixerView::MixerView(JackDawProject *project, const BMessenger &main)
    : BView("mixer", B_WILL_DRAW | B_FRAME_EVENTS), m_project(project), m_main(main),
      m_hscroll(NULL), m_master(NULL), m_scroll_x(0.0f), m_built(false)
{
    m_master = new MixerStripView(project, NULL, main); // NULL = master strip
    AddChild(m_master);
    m_hscroll = new MixerHScrollBar(this);
    AddChild(m_hscroll);
    SetExplicitMinSize(BSize(kStripW * 2.0f, 200.0f));
}

void MixerView::AttachedToWindow()
{
    BView::AttachedToWindow();
    SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), 1.04f));
    Rebuild();
}

void MixerView::Rebuild()
{
    // Drop the old per-track strips (master stays, always first/leftmost).
    for (MixerStripView *s : m_strips) {
        RemoveChild(s);
        delete s;
    }
    m_strips.clear();

    guint n = jackdaw_project_track_count(m_project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t = jackdaw_project_get_track(m_project, i);
        MixerStripView *s = new MixerStripView(m_project, t, m_main);
        AddChild(s);
        m_strips.push_back(s);
    }
    m_built = true;
    Relayout();
}

void MixerView::FrameResized(float newWidth, float newHeight)
{
    BView::FrameResized(newWidth, newHeight);
    Relayout();
}

void MixerView::Relayout()
{
    if (!m_master)
        return;

    const float sbH = B_H_SCROLL_BAR_HEIGHT;
    float contentH = Bounds().Height() - sbH;
    if (contentH < 1.0f)
        contentH = 1.0f;

    // Master first, then the per-track strips, all fixed width and full height.
    float x = -m_scroll_x;
    m_master->MoveTo(x, 0.0f);
    m_master->ResizeTo(kStripW - 1.0f, contentH);
    x += kStripW;
    for (MixerStripView *s : m_strips) {
        s->MoveTo(x, 0.0f);
        s->ResizeTo(kStripW - 1.0f, contentH);
        x += kStripW;
    }

    if (m_hscroll) {
        m_hscroll->MoveTo(0.0f, contentH);
        m_hscroll->ResizeTo(Bounds().Width(), sbH);
    }
    UpdateHScrollBar();
}

void MixerView::UpdateHScrollBar()
{
    if (!m_hscroll)
        return;
    float total = (float)(m_strips.size() + 1) * kStripW; // +1 for master
    float visible = Bounds().Width();
    float maxx = total - visible;
    if (maxx < 0.0f)
        maxx = 0.0f;
    m_hscroll->SetRange(0.0f, maxx);
    m_hscroll->SetProportion(total > 0.0f ? std::min(1.0f, visible / total) : 1.0f);
    m_hscroll->SetSteps(kStripW * 0.25f, visible > 0.0f ? visible : kStripW);
    if (m_scroll_x > maxx)
        SetScrollX(maxx); // re-clamp; will reposition through the scrollbar
}

void MixerView::SetScrollX(float x)
{
    if (x < 0.0f)
        x = 0.0f;
    if (x == m_scroll_x)
        return;
    m_scroll_x = x;
    if (!m_master)
        return;
    float px = -m_scroll_x;
    m_master->MoveTo(px, 0.0f);
    px += kStripW;
    for (MixerStripView *s : m_strips) {
        s->MoveTo(px, 0.0f);
        px += kStripW;
    }
}

void MixerView::Sync()
{
    if (!m_built)
        return;
    for (MixerStripView *s : m_strips)
        s->Sync();
    if (m_master)
        m_master->Sync();
}

void MixerView::UpdateMeters()
{
    if (!m_built)
        return;
    for (MixerStripView *s : m_strips)
        s->UpdateMeter();
    if (m_master)
        m_master->UpdateMeter();
}
