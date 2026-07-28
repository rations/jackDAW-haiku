#pragma once

#include <Application.h>

#include "engine/project.h"

class MainWindow;

class JackDawApp : public BApplication
{
public:
    explicit JackDawApp(JackDawProject *project);

    void ReadyToRun() override;
    bool QuitRequested() override;

private:
    // Catalog scan behind a progress window, before the main window is shown.
    void RunStartupScan();

    JackDawProject *m_project; // borrowed; main() owns the reference
    MainWindow *m_main_window;
};
