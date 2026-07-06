#include "JackDawApp.h"

#include <glib.h>

#include "engine/jackdaw-engine.h"
#include "MainWindow.h"

JackDawApp::JackDawApp(JackDawProject *project)
    : BApplication("application/x-vnd.jackdaw"), m_project(project), m_main_window(NULL)
{
}

void JackDawApp::ReadyToRun()
{
    // Construct the window first so its engine-event hook is in place, then
    // bring the engine up, then show. All of this runs on the app thread; the
    // window's looper thread starts at Show().
    m_main_window = new MainWindow(m_project);
    if (jackdaw_engine_init(m_project))
        g_warning("JACK engine failed to start — running without audio");
    m_main_window->Show();
}

bool JackDawApp::QuitRequested()
{
    return BApplication::QuitRequested();
}
