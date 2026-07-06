#pragma once

#include <Application.h>

class MainWindow;

class JackDawApp : public BApplication
{
public:
    JackDawApp();

    void ReadyToRun() override;
    bool QuitRequested() override;

private:
    MainWindow *m_main_window;
};
