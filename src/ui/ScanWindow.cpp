#include "ScanWindow.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <ListItem.h>
#include <ListView.h>
#include <Messenger.h>
#include <ScrollView.h>
#include <StatusBar.h>
#include <String.h>
#include <StringView.h>

#include <string.h>

// Internal messages.
enum {
    MSG_SCAN_PROGRESS = 'scpr', // string "plugin": one path is being described
    MSG_NEWPLUGINS_OK = 'npok',
};

/* ---------------------------------------------------------------- ScanWindow */

ScanWindow::ScanWindow(guint total)
    : BWindow(BRect(0, 0, 400, 100), "Scanning Plug-ins", B_MODAL_WINDOW_LOOK,
              B_MODAL_APP_WINDOW_FEEL,
              B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS),
      m_bar(NULL), m_label(NULL), m_total(total), m_done(0)
{
    BStringView *head = new BStringView("head", "Scanning for plug-ins\xE2\x80\xA6");

    m_bar = new BStatusBar("scan_bar");
    // 0 candidates would make every update a division by zero in the bar; a
    // max of 1 simply leaves it empty, which is the honest display for a
    // machine with no plug-ins installed.
    m_bar->SetMaxValue(total > 0 ? (float)total : 1.0f);

    m_label = new BStringView("scan_path", "");
    m_label->SetExplicitMinSize(BSize(360, B_SIZE_UNSET));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(head)
        .Add(m_bar)
        .Add(m_label);

    CenterOnScreen();
}

void ScanWindow::ProgressHook(const char *plugin, void *user)
{
    BMessenger *to = (BMessenger *)user;
    if (!to || !plugin)
        return;
    BMessage msg(MSG_SCAN_PROGRESS);
    msg.AddString("plugin", plugin);
    // Async, zero timeout: the scanning thread must never block on the window.
    to->SendMessage(&msg, (BHandler *)NULL, 0);
}

void ScanWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_SCAN_PROGRESS: {
            const char *path = NULL;
            if (message->FindString("plugin", &path) != B_OK || !path)
                break;
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            // The count is a lower bound (LV2 is not enumerated up front), so
            // clamp rather than letting the bar run past full.
            if (m_total > 0 && m_done < m_total)
                m_done++;
            m_bar->SetTo(m_total > 0 ? (float)m_done : 0.0f, base);
            break;
        }
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool ScanWindow::QuitRequested()
{
    return true;
}

/* ---------------------------------------------------------- NewPluginsWindow */

NewPluginsWindow::NewPluginsWindow(const GList *names, sem_id done)
    : BWindow(BRect(0, 0, 380, 320), "New Plug-ins", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
      m_done(done), m_released(false)
{
    guint n = g_list_length((GList *)names);
    BString headline;
    headline.SetToFormat("%u new plug-in%s found:", n, n == 1 ? "" : "s");
    BStringView *head = new BStringView("head", headline.String());

    BListView *list = new BListView("new_list", B_SINGLE_SELECTION_LIST);
    for (const GList *l = names; l; l = l->next) {
        const char *nm = (const char *)l->data;
        if (nm)
            list->AddItem(new BStringItem(nm));
    }
    BScrollView *scroll = new BScrollView("new_scroll", list, 0, false, true);

    BButton *ok = new BButton("ok", "OK", new BMessage(MSG_NEWPLUGINS_OK));
    ok->MakeDefault(true);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(head)
        .Add(scroll)
        .AddGroup(B_HORIZONTAL)
        .AddGlue()
        .Add(ok)
        .End();

    CenterOnScreen();
}

// Idempotent: the caller is blocked on this semaphore, so it must be released
// exactly once whether the user pressed OK or closed the window.
void NewPluginsWindow::ReleaseDone()
{
    if (m_released)
        return;
    m_released = true;
    if (m_done >= 0)
        release_sem(m_done);
}

void NewPluginsWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_NEWPLUGINS_OK:
            PostMessage(B_QUIT_REQUESTED);
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool NewPluginsWindow::QuitRequested()
{
    ReleaseDone();
    return true;
}
