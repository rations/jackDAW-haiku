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
    JackDawProject *m_project; // borrowed; main() owns the reference
    MainWindow *m_main_window;
};
