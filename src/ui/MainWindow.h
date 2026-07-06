#pragma once

#include <Window.h>

#include "engine/project.h"

class BMessageRunner;
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
    void TransportPlay();
    void TransportStop();
    void TransportRecord();
    void TransportToggle();

    JackDawProject *m_project; // borrowed; main() owns the reference

    TransportView *m_transport;
    TimelineView *m_timeline;
    BMessageRunner *m_tick_runner;
};
