#pragma once

#include <Window.h>

#include "engine/project.h"

class BMessageRunner;
class BStringView;
class TransportView;

class MainWindow : public BWindow
{
public:
    explicit MainWindow(JackDawProject *project);
    ~MainWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    // Call once after jackdaw_engine_init so the status line shows the real
    // sample rate / buffer size (runs on the creating thread, pre-Show).
    void UpdateEngineStatus();

private:
    void TransportPlay();
    void TransportStop();
    void TransportRecord();
    void TransportToggle();

    JackDawProject *m_project; // borrowed; main() owns the reference

    TransportView *m_transport;
    BStringView *m_status_view;
    BMessageRunner *m_tick_runner;
};
