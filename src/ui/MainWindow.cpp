#include "MainWindow.h"

#include <Application.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <TextView.h>

#include <stdio.h>
#include <stdlib.h>

#include "engine/jackdaw-engine.h"
#include "Messages.h"
#include "TimelineView.h"
#include "TransportView.h"

// UI refresh cadence (playhead readout, later ruler/lane playhead redraws).
static const bigtime_t kTickIntervalUsec = 33000; // ~30 Hz

// Target for engine events. Written only from the creating thread while the
// engine hook is unset (constructor / QuitRequested), read by JACK
// notification threads through the hook — so there is never a concurrent
// write. The object itself has process lifetime; a send to a dead window
// fails harmlessly.
static BMessenger g_engine_messenger;

// Runs on JACK notification threads: non-blocking post only (timeout 0);
// dropping the message is fine, handlers re-read engine state.
static void engine_event_hook(int event, void *user)
{
    (void)user;
    uint32 what;
    switch (event) {
        case JACKDAW_ENGINE_EVENT_PORTS_CHANGED:
            what = MSG_ENGINE_PORTS_CHANGED;
            break;
        case JACKDAW_ENGINE_EVENT_CONNECTIONS_CHANGED:
            what = MSG_ENGINE_CONNECTIONS_CHANGED;
            break;
        case JACKDAW_ENGINE_EVENT_SHUTDOWN:
            what = MSG_ENGINE_SHUTDOWN;
            break;
        default:
            return;
    }
    BMessage msg(what);
    g_engine_messenger.SendMessage(&msg, (BHandler *)NULL, 0);
}

// Window-level transport keys. Swallows the plain key so a focused BButton
// doesn't also react to space — but leaves everything alone while the user is
// typing in a text control (track names, BPM).
class TransportKeyFilter : public BMessageFilter
{
public:
    TransportKeyFilter() : BMessageFilter(B_KEY_DOWN)
    {
    }

    filter_result Filter(BMessage *message, BHandler **target) override
    {
        (void)target;
        BWindow *window = static_cast<BWindow *>(Looper());
        if (dynamic_cast<BTextView *>(window->CurrentFocus()) != NULL)
            return B_DISPATCH_MESSAGE; /* typing in a text field */

        int8 byte;
        if (message->FindInt8("byte", &byte) != B_OK)
            return B_DISPATCH_MESSAGE;

        switch (byte) {
            case B_SPACE:
                window->PostMessage(MSG_TRANSPORT_TOGGLE);
                return B_SKIP_MESSAGE;
            case B_HOME:
                window->PostMessage(MSG_TRANSPORT_RTZ);
                return B_SKIP_MESSAGE;
            default:
                return B_DISPATCH_MESSAGE;
        }
    }
};

// Clicking anywhere that is not the focused text field releases that field's
// focus and selection — window-wide, so a stuck BPM/timesig entry can't
// swallow the transport keys.
class FocusReleaseFilter : public BMessageFilter
{
public:
    FocusReleaseFilter() : BMessageFilter(B_MOUSE_DOWN)
    {
    }

    filter_result Filter(BMessage *message, BHandler **target) override
    {
        (void)message;
        BWindow *window = static_cast<BWindow *>(Looper());
        BTextView *focus = dynamic_cast<BTextView *>(window->CurrentFocus());
        if (focus != NULL && *target != focus) {
            focus->MakeFocus(false);
            focus->Select(0, 0);
        }
        return B_DISPATCH_MESSAGE;
    }
};

MainWindow::MainWindow(JackDawProject *project)
    : BWindow(BRect(100, 100, 1100, 700), "JackDAW", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
      m_project(project), m_transport(NULL), m_timeline(NULL), m_tick_runner(NULL)
{
    m_transport = new TransportView(project);
    m_timeline = new TimelineView(project);

    // No JACK status bar: server health/xrun monitoring is the patchbay's job
    // (JackGraph), matching the Linux JackDAW main window.
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(m_transport).Add(m_timeline);

    // Zoom shortcuts (Command is Alt on the default Haiku keymap).
    AddShortcut('=', B_COMMAND_KEY, new BMessage(MSG_ZOOM_IN));
    AddShortcut('+', B_COMMAND_KEY, new BMessage(MSG_ZOOM_IN));
    AddShortcut('-', B_COMMAND_KEY, new BMessage(MSG_ZOOM_OUT));

    AddCommonFilter(new TransportKeyFilter());
    AddCommonFilter(new FocusReleaseFilter());

    // Diagnostic switch: JACKDAW_NO_TICK=1 disables the periodic UI refresh,
    // to isolate whether the 30 Hz redraw traffic is implicated in xruns.
    if (getenv("JACKDAW_NO_TICK") == NULL)
        m_tick_runner =
            new BMessageRunner(BMessenger(this), BMessage(MSG_UI_TICK), kTickIntervalUsec);

    // Register for engine events before the engine is initialised so none are
    // missed (both happen on the creating thread, pre-Show).
    g_engine_messenger = BMessenger(this);
    jackdaw_engine_set_event_hook(engine_event_hook, NULL);
}

MainWindow::~MainWindow()
{
    delete m_tick_runner;
}

void MainWindow::TransportPlay()
{
    if (!jackdaw_engine_begin_countin(jackdaw_project_get_countin_before_play(m_project), FALSE))
        jackdaw_engine_start_playback();
}

void MainWindow::TransportStop()
{
    jackdaw_engine_stop_recording();
    jackdaw_engine_stop_playback();
}

void MainWindow::TransportRecord()
{
    if (!jackdaw_engine_begin_countin(jackdaw_project_get_countin_before_record(m_project), TRUE))
        jackdaw_engine_start_recording();
}

void MainWindow::TransportToggle()
{
    if (jackdaw_engine_is_playing() || jackdaw_engine_is_counting_in())
        TransportStop();
    else
        TransportPlay();
}

void MainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_UI_TICK:
            m_transport->UpdateReadout();
            m_timeline->TickUpdate();
            break;

        case MSG_ZOOM_IN:
            m_timeline->ZoomBy(0.5, m_timeline->LaneWidth() / 2);
            break;
        case MSG_ZOOM_OUT:
            m_timeline->ZoomBy(2.0, m_timeline->LaneWidth() / 2);
            break;

        case MSG_TRANSPORT_PLAY:
            TransportPlay();
            break;
        case MSG_TRANSPORT_STOP:
            TransportStop();
            break;
        case MSG_TRANSPORT_RECORD:
            TransportRecord();
            break;
        case MSG_TRANSPORT_TOGGLE:
            TransportToggle();
            break;
        case MSG_TRANSPORT_RTZ:
            jackdaw_engine_locate(0);
            break;

        case MSG_ENGINE_PORTS_CHANGED:
        case MSG_ENGINE_CONNECTIONS_CHANGED:
        case MSG_ENGINE_SHUTDOWN:
            // Port selectors refresh here once track strips exist; a server
            // shutdown alert can hang off MSG_ENGINE_SHUTDOWN later.
            break;

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MainWindow::QuitRequested()
{
    // Stop engine events reaching a window that is about to die. The engine
    // itself is torn down by main() after the app loop exits.
    jackdaw_engine_set_event_hook(NULL, NULL);
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}
