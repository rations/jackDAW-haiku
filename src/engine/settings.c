#include <glib.h>
#include <FindDirectory.h>
#include <StorageDefs.h>

#include "settings.h"

static GKeyFile *kf = NULL;
static gchar *path = NULL;

void settings_init(void)
{
    /* Haiku's per-user settings tree (usually /boot/home/config/settings),
     * resolved through find_directory rather than a hidden dot-directory in
     * $HOME. The volume argument is ignored for this directory constant. */
    char base[B_PATH_NAME_LENGTH];
    if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, base, sizeof(base)) != B_OK)
        g_strlcpy(base, "/boot/home/config/settings", sizeof(base));

    gchar *dir = g_build_filename(base, "JackDAW", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    path = g_build_filename(base, "JackDAW", "settings", NULL);
    kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
}

void settings_save(void)
{
    if (!kf || !path)
        return;
    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, NULL);
    if (data) {
        g_file_set_contents(path, data, (gssize)len, NULL);
        g_free(data);
    }
}

void settings_quit(void)
{
    settings_save();
    if (kf) {
        g_key_file_free(kf);
        kf = NULL;
    }
    if (path) {
        g_free(path);
        path = NULL;
    }
}

guint32 settings_get_uint32(const gchar *key, guint32 def)
{
    if (!kf)
        return def;
    GError *err = NULL;
    gint64 v = g_key_file_get_int64(kf, "jackdaw", key, &err);
    if (err) {
        g_error_free(err);
        return def;
    }
    return (guint32)v;
}

void settings_set_uint32(const gchar *key, guint32 val)
{
    if (!kf)
        return;
    g_key_file_set_int64(kf, "jackdaw", key, (gint64)val);
}

gchar *settings_get_string(const gchar *key, const gchar *def)
{
    if (!kf)
        return g_strdup(def ? def : "");
    GError *err = NULL;
    gchar *v = g_key_file_get_string(kf, "jackdaw", key, &err);
    if (err) {
        g_error_free(err);
        return g_strdup(def ? def : "");
    }
    return v;
}

void settings_set_string(const gchar *key, const gchar *val)
{
    if (!kf)
        return;
    g_key_file_set_string(kf, "jackdaw", key, val ? val : "");
}
