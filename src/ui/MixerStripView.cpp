#include "MixerStripView.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <StringView.h>

#include "engine/jackdaw-engine.h"
#include "FaderView.h"
#include "KnobView.h"
#include "Messages.h"
#include "VuView.h"

enum {
    MSG_M_MUTE = 'mmut',
    MSG_M_SOLO = 'msol',
    MSG_M_PAN = 'mpan',
    MSG_M_FADER = 'mfad',
};

MixerStripView::MixerStripView(JackDawProject *project, JackDawTrack *track, const BMessenger &main)
    : BView("mixer_strip", B_WILL_DRAW), m_project(project), m_track(track), m_main(main),
      m_name(NULL), m_mute(NULL), m_solo(NULL), m_pan(NULL), m_fader(NULL), m_vu(NULL)
{
    const char *label = track ? jackdaw_track_get_name(track) : "Master";
    m_name = new BStringView("name", label);
    m_name->SetAlignment(B_ALIGN_CENTER);
    // Ellipsize long names and cap the width so a long name can never force the
    // fixed-width strip wider (which would clip the controls off the right edge).
    m_name->SetTruncation(B_TRUNCATE_END);
    m_name->SetExplicitMinSize(BSize(20.0f, B_SIZE_UNSET));
    m_name->SetExplicitMaxSize(BSize(kStripInnerW, B_SIZE_UNSET));

    float fader_gain =
        track ? jackdaw_track_get_fader(track) : jackdaw_project_get_master_volume(project);
    m_fader = new FaderView("fader", fader_gain, new BMessage(MSG_M_FADER));
    m_vu = new VuView("vu", 16.0f);

    // Vertical stack, matching the Linux mixer strip order: name, pan, the
    // fader+VU block (which expands to fill the height), then Mute/Solo pinned at
    // the bottom. Each row is centred with glue so nothing is pushed off-edge.
    BLayoutBuilder::Group<> col(this, B_VERTICAL, 4.0f);
    col.SetInsets(5.0f, 4.0f, 5.0f, 4.0f);
    col.Add(m_name);

    if (track) {
        m_pan = new KnobView("P", -1.0, 1.0, (double)jackdaw_track_get_pan(track), 0.0,
                             KnobView::KIND_PAN, new BMessage(MSG_M_PAN));
        m_pan->SetCenterLabel("P");
        col.AddGroup(B_HORIZONTAL, 0.0f).AddGlue().Add(m_pan).AddGlue().End();
    }

    col.AddGroup(B_HORIZONTAL, 2.0f).AddGlue().Add(m_fader).Add(m_vu).AddGlue().End();

    // Mute is present on every strip including the master bus; Solo is a real-
    // track concept only (soloing the master would be a no-op against the solo
    // bus), matching the Linux mixer strip.
    BSize btn(26.0f, 22.0f); // fix min+max so the buttons never widen the strip
    m_mute = new BButton("M", "M", new BMessage(MSG_M_MUTE));
    m_mute->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_mute->SetExplicitMinSize(btn);
    m_mute->SetExplicitMaxSize(btn);
    if (track) {
        m_solo = new BButton("S", "S", new BMessage(MSG_M_SOLO));
        m_solo->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
        m_solo->SetExplicitMinSize(btn);
        m_solo->SetExplicitMaxSize(btn);
        col.AddGroup(B_HORIZONTAL, 3.0f).AddGlue().Add(m_mute).Add(m_solo).AddGlue().End();
    } else {
        col.AddGroup(B_HORIZONTAL, 3.0f).AddGlue().Add(m_mute).AddGlue().End();
    }

    col.End();

    SetExplicitMinSize(BSize(kMixerStripW, 200.0f));
    SetExplicitMaxSize(BSize(kMixerStripW, B_SIZE_UNLIMITED));
}

void MixerStripView::AttachedToWindow()
{
    BView::AttachedToWindow();
    if (Parent())
        SetViewColor(Parent()->ViewColor());
    m_fader->SetTarget(this);
    if (m_mute)
        m_mute->SetTarget(this);
    if (m_solo)
        m_solo->SetTarget(this);
    if (m_pan)
        m_pan->SetTarget(this);
    Sync();
}

void MixerStripView::Draw(BRect updateRect)
{
    (void)updateRect;
    // Divider on the right edge between channels.
    BRect b = Bounds();
    SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
    StrokeLine(BPoint(b.right, b.top), BPoint(b.right, b.bottom));
}

void MixerStripView::Sync()
{
    if (m_track) {
        m_name->SetText(jackdaw_track_get_name(m_track));
        if (m_mute)
            m_mute->SetValue(jackdaw_track_is_muted(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
        if (m_solo)
            m_solo->SetValue(jackdaw_track_is_soloed(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
        if (m_pan)
            m_pan->SetValue((double)jackdaw_track_get_pan(m_track));
        m_fader->SetGain(jackdaw_track_get_fader(m_track));
    } else {
        m_fader->SetGain(jackdaw_project_get_master_volume(m_project));
        if (m_mute)
            m_mute->SetValue(jackdaw_project_get_master_muted(m_project) ? B_CONTROL_ON
                                                                         : B_CONTROL_OFF);
    }
}

void MixerStripView::UpdateMeter()
{
    gfloat l = 0.0f, r = 0.0f;
    if (m_track)
        jackdaw_track_get_peaks(m_track, &l, &r);
    else
        jackdaw_engine_get_master_peaks(&l, &r);
    m_vu->SetPeaks(l, r);
}

void MixerStripView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_M_FADER: {
            float gain = 1.0f;
            message->FindFloat("gain", &gain);
            BMessage m(MSG_MIX_SET_FADER);
            m.AddInt32("slot", Slot());
            m.AddFloat("gain", gain);
            m_main.SendMessage(&m);
            break;
        }
        case MSG_M_PAN: {
            float pan = 0.0f;
            message->FindFloat("value", &pan);
            BMessage m(MSG_MIX_SET_PAN);
            m.AddInt32("slot", Slot());
            m.AddFloat("pan", pan);
            m_main.SendMessage(&m);
            break;
        }
        case MSG_M_MUTE: {
            BMessage m(MSG_MIX_TOGGLE_MUTE);
            m.AddInt32("slot", Slot());
            m.AddBool("on", m_mute->Value() == B_CONTROL_ON);
            m_main.SendMessage(&m);
            break;
        }
        case MSG_M_SOLO: {
            BMessage m(MSG_MIX_TOGGLE_SOLO);
            m.AddInt32("slot", Slot());
            m.AddBool("on", m_solo->Value() == B_CONTROL_ON);
            m_main.SendMessage(&m);
            break;
        }
        default:
            BView::MessageReceived(message);
            break;
    }
}
