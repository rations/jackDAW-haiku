#pragma once

#include <OS.h>
#include <Window.h>

#include <glib.h>

class BStatusBar;
class BStringView;

// Progress window shown while the plugin catalog is (re)scanned.
//
// Unlike the Linux original — which has to pump its GTK loop from inside the
// progress callback because the scan blocks its single UI thread — this window
// runs on its own looper and repaints itself. The scanning thread therefore
// only POSTS progress here and must never lock this window, or the two would be
// coupled again. Create, Show(), scan, then PostMessage(B_QUIT_REQUESTED).
class ScanWindow : public BWindow
{
public:
    // total = pluginhost_scan_candidate_count(); 0 gives an indeterminate bar.
    explicit ScanWindow(guint total);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    // pluginhost_set_scan_progress() callback. `user` is a heap BMessenger for
    // the window (the scan thread must not touch the window itself).
    static void ProgressHook(const char *plugin, void *user);

private:
    BStatusBar *m_bar;
    BStringView *m_label;
    guint m_total;
    guint m_done;
};

// Post-scan list of plug-ins that are new since the previous run. Runs its own
// looper; the caller blocks on `done` until the user dismisses it.
class NewPluginsWindow : public BWindow
{
public:
    // `names` is the GList of char* from pluginhost_scan_report_new(); it is
    // only read here (the caller still owns and frees it). `done` is released
    // once, when this window is dismissed.
    NewPluginsWindow(const GList *names, sem_id done);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

private:
    void ReleaseDone();

    sem_id m_done;
    bool m_released;
};
