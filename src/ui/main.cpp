#include <cstdlib>

#include "engine/glib_check.h"
#include "JackDawApp.h"

int main()
{
    // Verify the glib2 runtime (GObject + atomics) before any engine or UI
    // object exists; the engine/model layer is built on it.
    if (jackdaw_glib_check() != 0)
        return EXIT_FAILURE;

    JackDawApp app;
    app.Run();
    return EXIT_SUCCESS;
}
