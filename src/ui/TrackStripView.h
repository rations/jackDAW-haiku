#pragma once

#include <Messenger.h>
#include <View.h>

#include "engine/project.h"
#include "engine/track.h"

class BButton;
class BMenuField;
class BPopUpMenu;
class BTextControl;
class KnobView;
class VuView;

// One track's header strip (the 235 px column left of its timeline lane): name
// entry; Arm/Mute/Solo/Mono-Stereo/Fx buttons (Fx disabled — no plugins on
// Haiku yet); trim (V) and pan (P) knobs; a JACK capture-port input selector;
// and a stereo VU. It lives on the MainWindow looper, so it calls the track /
// engine API directly; structural actions (select, delete, context menu) go to
// MainWindow. SyncFromTrack() re-reads engine state after external changes.
class TrackStripView : public BView
{
public:
    TrackStripView(JackDawProject *project, JackDawTrack *track, const BMessenger &main);

    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MessageReceived(BMessage *message) override;

    JackDawTrack *Track() const
    {
        return m_track;
    }
    void SetPeaks(float l, float r); // called on the UI tick
    void SyncFromTrack();            // refresh controls from track/engine state

private:
    void BuildInputMenu();
    void ApplyStereo(bool stereo);

    JackDawProject *m_project; // borrowed
    JackDawTrack *m_track;     // borrowed (project owns the ref)
    BMessenger m_main;

    BTextControl *m_name;
    BButton *m_arm;
    BButton *m_mute;
    BButton *m_solo;
    BButton *m_stereo; // Mo <-> St
    BButton *m_fx;     // disabled/greyed
    KnobView *m_vol;   // trim (V)
    KnobView *m_pan;   // pan (P)
    BMenuField *m_input_field;
    BPopUpMenu *m_input_menu;
    VuView *m_vu;

    gulong m_state_handler;   // track "state-changed" -> SyncFromTrack
    gulong m_routing_handler; // track "routing-changed" -> SyncFromTrack
};
