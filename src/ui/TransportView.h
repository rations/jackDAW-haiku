#pragma once

#include <View.h>

#include "engine/project.h"

class BButton;
class BCheckBox;
class BStringView;
class ContextButton;
class StateButton;
class StepperControl;

// Transport bar (two rows, fixed metrics so it never re-flows on state change):
//
//   row 1  |◀ |<< >>| ▶|   ▶ ⟳ || ■ ●    <position>  <state>
//   row 2  Split  Grid Snap Metro Bars   BPM[-/+]  Sig[-/+]/[-/+]     Mixer
//
// Momentary transport buttons post MSG_TRANSPORT_* to the window (the sole
// engine caller). Tempo/grid/metronome controls apply to the project directly
// on this view (same looper). Right-clicking Record / Metro / Mixer posts a
// MSG_*_MENU with a screen point so the window can run the context popup.
class TransportView : public BView
{
public:
    explicit TransportView(JackDawProject *project);

    void AttachedToWindow() override;
    void MessageReceived(BMessage *message) override;
    // Clicking the bar background drops text-field focus; clicking the position
    // readout cycles the timecode format.
    void MouseDown(BPoint where) override;

    // Called by the window's UI tick and after timing changes.
    void UpdateReadout();
    // Re-read BPM/timesig/toggles from the project into the controls.
    void SyncControls();

private:
    JackDawProject *m_project; // borrowed

    BButton *m_rtz_button;
    BButton *m_step_back;
    BButton *m_step_fwd;
    BButton *m_next_button;
    StateButton *m_play_button;
    StateButton *m_loop_button;
    BButton *m_pause_button;
    BButton *m_stop_button;
    ContextButton *m_record_button;
    BStringView *m_readout;
    BStringView *m_state; // ▶ / ● / count-in marker, fixed width

    BButton *m_split_button;
    BCheckBox *m_grid;
    BCheckBox *m_snap;
    BCheckBox *m_bars_mode;
    ContextButton *m_metro_button;
    StepperControl *m_bpm;
    StepperControl *m_sig_num;
    StepperControl *m_sig_den;
    ContextButton *m_mixer_button;

    int m_timemode; // TIMEMODE_* used when the ruler is in time mode
};
