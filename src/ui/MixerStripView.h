#pragma once

#include <Messenger.h>
#include <View.h>

#include "engine/project.h"
#include "engine/track.h"

class BStringView;
class FaderView;
class KnobView;
class StateButton;
class VuView;

// Fixed channel-strip width (shared with MixerView's row layout) and the usable
// width inside the strip's insets — used to cap the name label so it ellipsizes
// instead of forcing the strip wider.
static const float kMixerStripW = 104.0f;
static const float kStripInnerW = 88.0f;

// One mixer channel: name, Mute/Solo, pan knob, dB fader, and a stereo VU. A
// master strip (track == NULL, slot -1) shows just a fader + VU. Edits are
// routed to MainWindow via BMessage (MSG_MIX_*) so the mixer works identically
// whether docked in the main window or in a detached window (single-mutator
// rule). Sync()/UpdateMeter() are driven by the host's tick — no GObject signal
// handlers, so a detached strip never touches views from the engine thread.
class MixerStripView : public BView
{
public:
    // track == NULL builds the master strip.
    MixerStripView(JackDawProject *project, JackDawTrack *track, const BMessenger &main);

    void AttachedToWindow() override;
    void Draw(BRect updateRect) override;
    void MessageReceived(BMessage *message) override;

    void Sync();        // reflect fader/pan/mute/solo/name from state
    void UpdateMeter(); // push fresh peaks into the VU

    JackDawTrack *Track() const
    {
        return m_track;
    }

private:
    int Slot() const
    {
        return m_track ? (int)m_track->slot : -1;
    }

    JackDawProject *m_project; // borrowed
    JackDawTrack *m_track;     // borrowed; NULL = master
    BMessenger m_main;

    BStringView *m_name;
    StateButton *m_mute;
    StateButton *m_solo;
    StateButton *m_fx; // NULL on master; blue when the track carries a chain
    KnobView *m_pan;
    FaderView *m_fader;
    VuView *m_vu;
};
