#pragma once

#include <Messenger.h>
#include <Window.h>

#include <vector>

#include "engine/midicontrol.h" // MidiCtlMapping (POD, carried in snapshots)

class BButton;
class BGroupView;
class BMenuField;
class StepperControl;

// Options -> MIDI Control: edits the control-surface mapping table (footswitch /
// CC -> FX bypass, mix, param, track mute, transport). Runs its own looper, so
// per the single-mutator rule it never touches the engine or the mapping table
// directly: MainWindow owns both. This window keeps a local snapshot (delivered
// by MSG_MIDICTL_SNAPSHOT), renders it, and posts every edit (add/remove/learn/
// field change/source) back to MainWindow, which applies it and returns a fresh
// snapshot. Closing hides the window; MainWindow keeps the singleton alive.
class MidiControlWindow : public BWindow
{
public:
    explicit MidiControlWindow(BMessenger target);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override; // hide, keep the singleton alive

    // MainWindow pushes the authoritative state here on every change.
    void ApplySnapshot(const BMessage *snap);

private:
    // One rendered mapping row; holds its controls so a field change can re-read
    // the whole row and post a complete mapping back to MainWindow.
    struct Row {
        MidiCtlMapping map; // cached values (trigger fields come from learn)
        BButton *learn;
        BMenuField *action;
        BMenuField *track;
        StepperControl *fx;
        StepperControl *param;
        StepperControl *group;
        BButton *remove;
    };

    void RebuildRows();      // tear down + rebuild from m_maps
    void PostRow(int index); // read row `index` controls -> MSG_MIDICTL_UPDATE
    BMenuField *BuildActionMenu(int index, uint8 action);
    BMenuField *BuildTrackMenu(int index, int32 track);

    BMessenger m_target;

    // Local snapshot (authoritative copy lives in MainWindow).
    std::vector<MidiCtlMapping> m_maps;
    std::vector<BString> m_track_names;
    std::vector<BString> m_ports;
    BString m_cur_source;
    int m_learn_index;

    BMenuField *m_source;   // control_in source picker
    BGroupView *m_rows_box; // vertical container the Row controls live in
    std::vector<Row> m_rows;
};
