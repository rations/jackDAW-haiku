#pragma once

#include <View.h>

#include "engine/project.h"

class BButton;
class BCheckBox;
class BStringView;
class BTextControl;

// Transport bar: play/stop/record/RTZ buttons, position readout (timecode or
// bars.beats.ticks per the project ruler mode), BPM + time signature entry,
// metronome and snap toggles, ruler-mode toggle.
//
// Buttons post MSG_TRANSPORT_* to the window (the sole engine caller); the
// tempo/toggle controls target this view, which applies them to the project
// directly — all of it on the owning window's looper thread.
class TransportView : public BView
{
public:
    explicit TransportView(JackDawProject *project);

    void AttachedToWindow() override;
    void MessageReceived(BMessage *message) override;
    // Clicking the bar's background takes focus away from any text control,
    // restoring the transport keys.
    void MouseDown(BPoint where) override;

    // Called by the window's UI tick and after timing changes.
    void UpdateReadout();
    // Re-read BPM/timesig/toggles from the project into the controls.
    void SyncControls();

private:
    JackDawProject *m_project; // borrowed

    BButton *m_rtz_button;
    BButton *m_play_button;
    BButton *m_stop_button;
    BButton *m_record_button;
    BStringView *m_readout;
    BStringView *m_state; // ▶ / ● / count-in marker, fixed width
    BTextControl *m_bpm;
    BTextControl *m_sig_num;
    BTextControl *m_sig_den;
    BCheckBox *m_metronome;
    BCheckBox *m_grid;
    BCheckBox *m_snap;
    BCheckBox *m_bars_mode;
};
