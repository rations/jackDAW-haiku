#ifndef MESSAGE_H_INCLUDED
#define MESSAGE_H_INCLUDED

#include <glib.h>

/* Display an error to the user. Currently logs via g_warning only.
 * TODO(flagged): route through the engine event hook so the UI can raise a
 * BAlert once the main window exists (kept minimal until a caller needs it). */
void jackdaw_error(const gchar *msg);

#endif /* MESSAGE_H_INCLUDED */
