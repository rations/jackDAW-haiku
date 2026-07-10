#pragma once

#include <Window.h>

#include <vector>

#include "engine/project.h"

class BFilePanel;
class BLayoutItem;
class BMenuBar;
class BMessageRunner;
class BPoint;
class CountInWindow;
class MetroVolumeWindow;
class MidiWindow;
class MixerView;
class MixerWindow;
class TimelineView;
class TransportView;

class MainWindow : public BWindow
{
public:
    explicit MainWindow(JackDawProject *project);
    ~MainWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    // Rebuild both mixers' channel strips (called from GObject track-list
    // signal trampolines, which run on this looper).
    void RebuildMixers();

    // Piano-roll editor registry. Editors acquire this window's looper lock
    // for model access, so this window must never lock them — communication
    // back is async PostMessage only. Unregister runs on the editor's thread
    // (its destructor) while holding this window's lock.
    void UnregisterMidiEditor(MidiWindow *w);

private:
    BMenuBar *BuildMenuBar();

    void TransportPlay();
    void TransportStop();
    void TransportRecord();
    void TransportToggle();
    void TransportPause();
    void StepCursor(int direction); // -1 / +1: nudge the cursor by 10 ms
    void LocateNextBoundary();      // ▶| — next clip boundary (loop edges until P6)

    // Track management (single-mutator: all engine/project edits run here).
    void AddTrack(JackDawTrackKind kind);
    void DeleteTrack(JackDawTrack *track);
    void ShowTrackContext(int slot, BPoint screen_where);
    // Import an audio file onto a fresh audio track (region placed at timeline 0).
    void LoadFileAsTrack(const char *path);

    // Right-click context popups (built fresh so their marks are current).
    void ShowRecordMenu(BPoint screen_where);
    void ShowMetroMenu(BPoint screen_where);
    void ShowMixerMenu(BPoint screen_where);

    // Metronome option windows (lazily created singletons, hidden not destroyed).
    void OpenMetroVolumeWindow();
    void OpenCountInWindow();

    // Piano-roll MIDI editor (per-instrument-track window; presents if open).
    void OpenMidiEditor(JackDawTrack *track);
    bool TrackInProject(JackDawTrack *track) const;

    // Mixer (docked pane + optional detached window).
    void ApplyMixerState();               // show/hide dock vs. window
    void SyncMixers();                    // per-tick control/meter refresh
    JackDawTrack *TrackForSlot(int slot); // map a mixer/strip slot to its track

    JackDawProject *m_project; // borrowed; main() owns the reference

    TransportView *m_transport;
    TimelineView *m_timeline;
    BMessageRunner *m_tick_runner;

    MetroVolumeWindow *m_metro_volume_window; // NULL until first opened
    CountInWindow *m_countin_window;          // NULL until first opened

    MixerView *m_mixer;          // docked pane
    BLayoutItem *m_mixer_item;   // its layout slot (for collapse)
    MixerWindow *m_mixer_window; // detached window (NULL until first detach)
    bool m_mixer_visible;        // user toggled the mixer on

    BFilePanel *m_load_panel; // "Load File as New Track" open panel (lazy)

    // Open piano-roll editors (guarded by this window's looper lock).
    std::vector<MidiWindow *> m_midi_editors;

    gulong m_track_added_h;
    gulong m_track_removed_h;
    gulong m_tracks_reordered_h;
};
