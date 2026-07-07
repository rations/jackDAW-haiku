#pragma once

#include <Window.h>

#include "engine/project.h"

class BMenuBar;
class BMessageRunner;
class BPoint;
class CountInWindow;
class MetroVolumeWindow;
class TimelineView;
class TransportView;

class MainWindow : public BWindow
{
public:
    explicit MainWindow(JackDawProject *project);
    ~MainWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

private:
    BMenuBar *BuildMenuBar();

    void TransportPlay();
    void TransportStop();
    void TransportRecord();
    void TransportToggle();
    void TransportPause();
    void StepCursor(int direction); // -1 / +1: nudge the cursor by 10 ms
    void LocateNextBoundary();      // ▶| — next clip boundary (loop edges until P6)

    // Right-click context popups (built fresh so their marks are current).
    void ShowRecordMenu(BPoint screen_where);
    void ShowMetroMenu(BPoint screen_where);
    void ShowMixerMenu(BPoint screen_where);

    // Metronome option windows (lazily created singletons, hidden not destroyed).
    void OpenMetroVolumeWindow();
    void OpenCountInWindow();

    JackDawProject *m_project; // borrowed; main() owns the reference

    TransportView *m_transport;
    TimelineView *m_timeline;
    BMessageRunner *m_tick_runner;

    MetroVolumeWindow *m_metro_volume_window; // NULL until first opened
    CountInWindow *m_countin_window;          // NULL until first opened
};
