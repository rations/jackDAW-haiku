#include "JackDawApp.h"

#include "MainWindow.h"

JackDawApp::JackDawApp() : BApplication("application/x-vnd.jackdaw"), m_main_window(NULL)
{
}

void JackDawApp::ReadyToRun()
{
    m_main_window = new MainWindow();
    m_main_window->Show();
}

bool JackDawApp::QuitRequested()
{
    // Engine teardown is wired in here (a normal application thread, never a
    // JACK callback context) once the engine exists.
    return BApplication::QuitRequested();
}
