#include <cstdlib>

#include <glib-object.h>

#include "engine/glib_check.h"
#include "engine/jackdaw-engine.h"
#include "engine/project.h"
#include "engine/settings.h"
#include "JackDawApp.h"

int main()
{
    // Verify the glib2 runtime (GObject + atomics) before any engine or UI
    // object exists; the engine/model layer is built on it.
    if (jackdaw_glib_check() != 0)
        return EXIT_FAILURE;

    settings_init();

    JackDawProject *project = jackdaw_project_new();

    {
        JackDawApp app(project);
        app.Run();
    }

    jackdaw_engine_quit();
    g_object_unref(project);
    settings_quit();

    return EXIT_SUCCESS;
}
