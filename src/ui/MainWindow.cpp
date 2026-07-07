#include "MainWindow.h"

#include <Application.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <LayoutItem.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <TextView.h>

#include <stdio.h>
#include <stdlib.h>

#include "engine/jackdaw-engine.h"
#include "engine/settings.h"
#include "Messages.h"
#include "MetronomeWindows.h"
#include "MixerView.h"
#include "MixerWindow.h"
#include "TimelineView.h"
#include "TransportView.h"

// GObject "track-added/removed/reordered" -> refresh both mixers. Emitted only
// on this (the MainWindow) looper, so touching the docked mixer is safe; the
// detached window is refreshed via a posted message on its own looper.
static void mainwin_tracks_changed_2(gpointer, gpointer, gpointer user)
{
    static_cast<MainWindow *>(user)->RebuildMixers();
}
static void mainwin_tracks_reordered(gpointer, gpointer user)
{
    static_cast<MainWindow *>(user)->RebuildMixers();
}

// UI refresh cadence (playhead readout, ruler/lane playhead redraws).
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
// doesn't also react — but leaves everything alone while the user is typing in
// a text control (track names, BPM).
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
            case B_LEFT_ARROW:
                window->PostMessage(MSG_TRANSPORT_STEP_BACK);
                return B_SKIP_MESSAGE;
            case B_RIGHT_ARROW:
                window->PostMessage(MSG_TRANSPORT_STEP_FWD);
                return B_SKIP_MESSAGE;
            case 'r':
            case 'R':
                window->PostMessage(MSG_TRANSPORT_RECORD);
                return B_SKIP_MESSAGE;
            case 'l':
            case 'L':
                window->PostMessage(MSG_TRANSPORT_LOOP);
                return B_SKIP_MESSAGE;
            case 's':
            case 'S':
                window->PostMessage(MSG_SPLIT);
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

// Add a menu item, optionally disabled until its feature phase lands.
static BMenuItem *AddItem(BMenu *menu, const char *label, uint32 what, char shortcut = 0,
                          uint32 mods = 0, bool enabled = true)
{
    BMenuItem *item = new BMenuItem(label, new BMessage(what), shortcut, mods);
    item->SetEnabled(enabled);
    menu->AddItem(item);
    return item;
}

MainWindow::MainWindow(JackDawProject *project)
    : BWindow(BRect(100, 100, 1100, 700), "JackDAW", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS),
      m_project(project), m_transport(NULL), m_timeline(NULL), m_tick_runner(NULL),
      m_metro_volume_window(NULL), m_countin_window(NULL), m_mixer(NULL), m_mixer_item(NULL),
      m_mixer_window(NULL), m_mixer_visible(false), m_track_added_h(0), m_track_removed_h(0),
      m_tracks_reordered_h(0)
{
    BMenuBar *menu_bar = BuildMenuBar();
    m_transport = new TransportView(project);
    m_timeline = new TimelineView(project);
    m_mixer = new MixerView(project, BMessenger(this));
    m_mixer->SetExplicitMinSize(BSize(B_SIZE_UNSET, 250.0f));
    m_mixer->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 320.0f));

    // No JACK status bar: server health/xrun monitoring is the patchbay's job
    // (JackGraph), matching the Linux JackDAW main window.
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(menu_bar).Add(m_transport).Add(m_timeline);
    m_mixer_item = GetLayout()->AddView(m_mixer); // docked pane below the timeline
    m_mixer_item->SetVisible(false);              // hidden until toggled on

    // Track-list changes rebuild the mixer channel strips.
    m_track_added_h =
        g_signal_connect(project, "track-added", G_CALLBACK(mainwin_tracks_changed_2), this);
    m_track_removed_h =
        g_signal_connect(project, "track-removed", G_CALLBACK(mainwin_tracks_changed_2), this);
    m_tracks_reordered_h =
        g_signal_connect(project, "tracks-reordered", G_CALLBACK(mainwin_tracks_reordered), this);

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
    if (m_track_added_h)
        g_signal_handler_disconnect(m_project, m_track_added_h);
    if (m_track_removed_h)
        g_signal_handler_disconnect(m_project, m_track_removed_h);
    if (m_tracks_reordered_h)
        g_signal_handler_disconnect(m_project, m_tracks_reordered_h);
    delete m_tick_runner;
}

void MainWindow::ApplyMixerState()
{
    bool in_window = settings_get_uint32("mixer_in_window", 0) != 0;
    bool want_dock = m_mixer_visible && !in_window;
    bool want_win = m_mixer_visible && in_window;

    if (m_mixer_item)
        m_mixer_item->SetVisible(want_dock);

    if (want_win) {
        if (m_mixer_window == NULL)
            m_mixer_window = new MixerWindow(m_project, BMessenger(this));
        if (m_mixer_window->LockLooper()) {
            m_mixer_window->Rebuild();
            if (m_mixer_window->IsHidden())
                m_mixer_window->Show();
            m_mixer_window->Activate();
            m_mixer_window->UnlockLooper();
        }
    } else if (m_mixer_window && m_mixer_window->LockLooper()) {
        if (!m_mixer_window->IsHidden())
            m_mixer_window->Hide();
        m_mixer_window->UnlockLooper();
    }
}

void MainWindow::RebuildMixers()
{
    if (m_mixer)
        m_mixer->Rebuild();
    if (m_mixer_window)
        BMessenger(m_mixer_window).SendMessage(MSG_MIXER_REBUILD);
}

void MainWindow::SyncMixers()
{
    bool in_window = settings_get_uint32("mixer_in_window", 0) != 0;
    if (m_mixer && m_mixer_visible && !in_window) {
        m_mixer->Sync();
        m_mixer->UpdateMeters();
    }
    // The detached window refreshes itself on its own runner tick.
}

JackDawTrack *MainWindow::TrackForSlot(int slot)
{
    if (slot < 0)
        return NULL; // master
    guint n = jackdaw_project_track_count(m_project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *t = jackdaw_project_get_track(m_project, i);
        if ((int)t->slot == slot)
            return t;
    }
    return NULL;
}

BMenuBar *MainWindow::BuildMenuBar()
{
    BMenuBar *bar = new BMenuBar("menubar");

    // File — session I/O lands in the save/load phase; render in the export
    // phase. Present but disabled until then.
    BMenu *file = new BMenu("File");
    AddItem(file, "Open Project…", MSG_FILE_OPEN, 'O', B_COMMAND_KEY, false);
    AddItem(file, "Save Project", MSG_FILE_SAVE, 'S', B_COMMAND_KEY, false);
    AddItem(file, "Save Project As…", MSG_FILE_SAVE_AS, 'S', B_COMMAND_KEY | B_SHIFT_KEY, false);
    file->AddSeparatorItem();
    AddItem(file, "Render…", MSG_FILE_RENDER, 0, 0, false);
    AddItem(file, "Render Region…", MSG_FILE_RENDER_REGION, 0, 0, false);
    file->AddSeparatorItem();
    AddItem(file, "New Session", MSG_FILE_NEW, 'N', B_COMMAND_KEY, false);
    file->AddSeparatorItem();
    AddItem(file, "Quit", B_QUIT_REQUESTED, 'Q', B_COMMAND_KEY);
    bar->AddItem(file);

    // Edit — the undo manager is a later-phase port (currently a no-op stub).
    BMenu *edit = new BMenu("Edit");
    AddItem(edit, "Undo", MSG_EDIT_UNDO, 'Z', B_COMMAND_KEY);
    AddItem(edit, "Redo", MSG_EDIT_REDO, 'Y', B_COMMAND_KEY);
    bar->AddItem(edit);

    // Track — add/delete are live now; Load File arrives with the clip phase.
    BMenu *track = new BMenu("Track");
    AddItem(track, "Add Empty Track", MSG_TRACK_ADD, 'T', B_COMMAND_KEY);
    AddItem(track, "Add MIDI Track", MSG_TRACK_ADD_MIDI, 'T', B_COMMAND_KEY | B_SHIFT_KEY);
    AddItem(track, "Load File as New Track…", MSG_TRACK_LOAD_FILE, 0, 0, false);
    track->AddSeparatorItem();
    AddItem(track, "Delete Active Track", MSG_TRACK_DELETE, 0, 0);
    bar->AddItem(track);

    // Transport — all functional now.
    BMenu *transport = new BMenu("Transport");
    AddItem(transport, "Play / Stop", MSG_TRANSPORT_TOGGLE);
    AddItem(transport, "Record", MSG_TRANSPORT_RECORD);
    AddItem(transport, "Loop", MSG_TRANSPORT_LOOP);
    transport->AddSeparatorItem();
    AddItem(transport, "Locate to Start", MSG_TRANSPORT_RTZ);
    bar->AddItem(transport);

    // View — zoom + metronome options now; the mixer-in-window setting is
    // stored now and honoured once the mixer view exists.
    BMenu *view = new BMenu("View");
    AddItem(view, "Zoom In", MSG_ZOOM_IN, '+', B_COMMAND_KEY);
    AddItem(view, "Zoom Out", MSG_ZOOM_OUT, '-', B_COMMAND_KEY);
    view->AddSeparatorItem();
    BMenuItem *mix_win = new BMenuItem("Open Mixer in Window", new BMessage(MSG_MIXER_OPEN_WINDOW));
    mix_win->SetMarked(settings_get_uint32("mixer_in_window", 0) != 0);
    view->AddItem(mix_win);
    view->AddSeparatorItem();
    AddItem(view, "Metronome…", MSG_METRO_VOLUME);
    bar->AddItem(view);

    // Options — I/O routing, plugins (excluded on Haiku), MIDI control surface.
    BMenu *options = new BMenu("Options");
    AddItem(options, "Inputs/Outputs…", MSG_OPT_IO, 0, 0, false);
    AddItem(options, "Plugins…", MSG_OPT_PLUGINS, 0, 0, false);
    AddItem(options, "MIDI Control…", MSG_OPT_MIDI_CONTROL, 0, 0, false);
    bar->AddItem(options);

    return bar;
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
    if (jackdaw_engine_is_recording() || jackdaw_engine_is_counting_in()) {
        TransportStop();
        return;
    }
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

void MainWindow::TransportPause()
{
    // Stop rolling but keep the play position (unlike RTZ / Stop-to-start).
    jackdaw_engine_stop_recording();
    jackdaw_engine_stop_playback();
}

void MainWindow::StepCursor(int direction)
{
    // One step = 10 ms (samplerate / 100), matching the Linux |<< / >>| buttons.
    jack_nframes_t sr = jackdaw_engine_get_sample_rate();
    off_t step = (off_t)(sr / 100);
    off_t pos = jackdaw_engine_get_play_pos();
    if (direction < 0)
        pos = (pos > step) ? pos - step : 0;
    else
        pos += step;
    m_timeline->LocateTo(pos);
}

void MainWindow::LocateNextBoundary()
{
    // Until clips exist (region-boundary scan lands with the clip phase), the
    // meaningful boundaries are the loop-region edges. Jump to the next one
    // after the cursor.
    off_t cursor = jackdaw_engine_get_play_pos();
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    off_t next = -1;
    if (jackdaw_engine_has_loop_region()) {
        if (ls > cursor)
            next = ls;
        else if (le > cursor)
            next = le;
    }
    if (next >= 0)
        m_timeline->LocateTo(next);
}

void MainWindow::AddTrack(JackDawTrackKind kind)
{
    JackDawTrack *t = jackdaw_track_new(NULL);
    jackdaw_track_set_kind(t, kind);
    // Engine first so track->slot is assigned before views build for it.
    if (jackdaw_engine_add_track(t)) {
        g_object_unref(t);
        return;
    }
    jackdaw_project_add_track(m_project, t); // takes its own ref
    jackdaw_project_select_single(m_project, t);
    g_object_unref(t);
}

void MainWindow::DeleteTrack(JackDawTrack *track)
{
    if (!track)
        return;
    // Clear the RT slot before dropping the last GObject ref.
    jackdaw_engine_remove_track(track);
    jackdaw_project_remove_track(m_project, track);
}

void MainWindow::ShowTrackContext(int slot, BPoint screen_where)
{
    JackDawTrack *t = NULL;
    guint n = jackdaw_project_track_count(m_project);
    for (guint i = 0; i < n; i++) {
        JackDawTrack *cand = jackdaw_project_get_track(m_project, i);
        if ((int)cand->slot == slot) {
            t = cand;
            break;
        }
    }
    if (!t)
        return;

    BPopUpMenu menu("track_context", false, false);
    BMenuItem *del = new BMenuItem("Delete Track", NULL);
    menu.AddItem(del);
    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == del)
        DeleteTrack(t);
}

void MainWindow::ShowRecordMenu(BPoint screen_where)
{
    BPopUpMenu menu("record_mode", false, false);
    BMenuItem *normal = new BMenuItem("Normal", new BMessage(MSG_RECORD_MODE_NORMAL));
    BMenuItem *punch = new BMenuItem("Punch In/Out", new BMessage(MSG_RECORD_MODE_PUNCH));
    int mode = jackdaw_engine_get_record_mode();
    normal->SetMarked(mode != RECORD_MODE_PUNCH);
    punch->SetMarked(mode == RECORD_MODE_PUNCH);
    menu.AddItem(normal);
    menu.AddItem(punch);

    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == normal)
        jackdaw_engine_set_record_mode(RECORD_MODE_NORMAL);
    else if (chosen == punch)
        jackdaw_engine_set_record_mode(RECORD_MODE_PUNCH);
}

void MainWindow::ShowMetroMenu(BPoint screen_where)
{
    BPopUpMenu menu("metro_options", false, false);
    BMenuItem *vol = new BMenuItem("Volume…", NULL);
    BMenuItem *ci = new BMenuItem("Count in…", NULL);
    BMenuItem *hp = new BMenuItem("Headphones only (click output)", NULL);
    hp->SetMarked(jackdaw_project_get_metronome_route(m_project) == METRONOME_ROUTE_CLICK_PORT);
    menu.AddItem(vol);
    menu.AddItem(ci);
    menu.AddSeparatorItem();
    menu.AddItem(hp);

    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == vol) {
        OpenMetroVolumeWindow();
    } else if (chosen == ci) {
        OpenCountInWindow();
    } else if (chosen == hp) {
        JackDawMetronomeRoute route =
            hp->IsMarked() ? METRONOME_ROUTE_MAIN : METRONOME_ROUTE_CLICK_PORT;
        jackdaw_project_set_metronome_route(m_project, route);
    }
}

void MainWindow::ShowMixerMenu(BPoint screen_where)
{
    BPopUpMenu menu("mixer_options", false, false);
    BMenuItem *in_window = new BMenuItem("Open in Window", NULL);
    in_window->SetMarked(settings_get_uint32("mixer_in_window", 0) != 0);
    menu.AddItem(in_window);

    BMenuItem *chosen = menu.Go(screen_where, false, false);
    if (chosen == in_window)
        PostMessage(MSG_MIXER_OPEN_WINDOW);
}

void MainWindow::OpenMetroVolumeWindow()
{
    if (m_metro_volume_window == NULL)
        m_metro_volume_window = new MetroVolumeWindow(BMessenger(this));
    // The option window runs its own looper; lock it before touching its views.
    if (m_metro_volume_window->LockLooper()) {
        m_metro_volume_window->SyncVolume(jackdaw_project_get_metronome_volume(m_project));
        if (m_metro_volume_window->IsHidden())
            m_metro_volume_window->Show();
        m_metro_volume_window->Activate();
        m_metro_volume_window->UnlockLooper();
    }
}

void MainWindow::OpenCountInWindow()
{
    if (m_countin_window == NULL)
        m_countin_window = new CountInWindow(BMessenger(this));
    if (m_countin_window->LockLooper()) {
        m_countin_window->SyncCountin((int)jackdaw_project_get_countin_before_record(m_project),
                                      (int)jackdaw_project_get_countin_before_play(m_project));
        if (m_countin_window->IsHidden())
            m_countin_window->Show();
        m_countin_window->Activate();
        m_countin_window->UnlockLooper();
    }
}

void MainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_UI_TICK:
            m_transport->UpdateReadout();
            m_timeline->TickUpdate();
            SyncMixers();
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
        case MSG_TRANSPORT_PAUSE:
            TransportPause();
            break;
        case MSG_TRANSPORT_RTZ:
            jackdaw_engine_locate(0);
            break;
        case MSG_TRANSPORT_STEP_BACK:
            StepCursor(-1);
            break;
        case MSG_TRANSPORT_STEP_FWD:
            StepCursor(+1);
            break;
        case MSG_TRANSPORT_NEXT_BOUNDARY:
            LocateNextBoundary();
            break;
        case MSG_TRANSPORT_LOOP:
            jackdaw_engine_set_loop_enabled(!jackdaw_engine_get_loop_enabled());
            m_timeline->InvalidateAll();
            break;

        case MSG_SPLIT:
            // Region splitting arrives with the clip-editing phase; no-op until
            // there are regions under the cursor to split.
            break;

        case MSG_EDIT_UNDO:
            jackdaw_project_undo(m_project);
            break;
        case MSG_EDIT_REDO:
            jackdaw_project_redo(m_project);
            break;

        case MSG_TRACK_ADD:
            AddTrack(JACKDAW_TRACK_AUDIO);
            break;
        case MSG_TRACK_ADD_MIDI:
            AddTrack(JACKDAW_TRACK_INSTRUMENT);
            break;
        case MSG_TRACK_DELETE:
            DeleteTrack(jackdaw_project_get_active_track(m_project));
            break;
        case MSG_TRACK_DELETE_SLOT:
        case MSG_TRACK_CONTEXT: {
            int32 slot = -1;
            message->FindInt32("slot", &slot);
            if (message->what == MSG_TRACK_DELETE_SLOT) {
                guint n = jackdaw_project_track_count(m_project);
                for (guint i = 0; i < n; i++) {
                    JackDawTrack *t = jackdaw_project_get_track(m_project, i);
                    if ((int)t->slot == slot) {
                        DeleteTrack(t);
                        break;
                    }
                }
            } else {
                BPoint where;
                if (message->FindPoint("screen_where", &where) == B_OK)
                    ShowTrackContext(slot, where);
            }
            break;
        }
        case MSG_TRACK_MOVE: {
            int32 from = -1, to = -1;
            message->FindInt32("from", &from);
            message->FindInt32("to", &to);
            if (from >= 0 && to >= 0)
                jackdaw_project_move_track(m_project, (guint)from, (guint)to);
            break;
        }

        // Right-click context popups (screen point carried in the message).
        case MSG_RECORD_MENU:
        case MSG_METRO_MENU:
        case MSG_MIXER_MENU: {
            BPoint where;
            if (message->FindPoint("screen_where", &where) != B_OK)
                break;
            if (message->what == MSG_RECORD_MENU)
                ShowRecordMenu(where);
            else if (message->what == MSG_METRO_MENU)
                ShowMetroMenu(where);
            else
                ShowMixerMenu(where);
            break;
        }

        case MSG_METRO_VOLUME:
            OpenMetroVolumeWindow();
            break;
        case MSG_METRO_COUNTIN:
            OpenCountInWindow();
            break;

        // Value changes applied on this looper (posted by the option windows).
        case MSG_METRO_SET_VOLUME: {
            float db = 0.0f;
            message->FindFloat("db", &db);
            jackdaw_project_set_metronome_volume(m_project, db);
            break;
        }
        case MSG_METRO_SET_COUNTIN_REC: {
            int32 beats = 0;
            message->FindInt32("beats", &beats);
            jackdaw_project_set_countin_before_record(m_project, (guint)(beats < 0 ? 0 : beats));
            break;
        }
        case MSG_METRO_SET_COUNTIN_PLAY: {
            int32 beats = 0;
            message->FindInt32("beats", &beats);
            jackdaw_project_set_countin_before_play(m_project, (guint)(beats < 0 ? 0 : beats));
            break;
        }

        case MSG_MIXER_TOGGLE:
            m_mixer_visible = !m_mixer_visible;
            ApplyMixerState();
            break;
        case MSG_MIXER_OPEN_WINDOW: {
            // Toggle docked <-> detached. Enabling the window also shows the
            // mixer if it was off; closing the window (which re-posts this)
            // flips back to docked.
            bool on = settings_get_uint32("mixer_in_window", 0) != 0;
            settings_set_uint32("mixer_in_window", on ? 0 : 1);
            if (!on)
                m_mixer_visible = true;
            ApplyMixerState();
            break;
        }

        // Mixer strip edits (docked or detached) applied here (single mutator).
        case MSG_MIX_SET_FADER: {
            int32 slot = -1;
            float gain = 1.0f;
            message->FindInt32("slot", &slot);
            message->FindFloat("gain", &gain);
            if (slot < 0)
                jackdaw_project_set_master_volume(m_project, gain);
            else if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_fader(t, gain);
            break;
        }
        case MSG_MIX_SET_PAN: {
            int32 slot = -1;
            float pan = 0.0f;
            message->FindInt32("slot", &slot);
            message->FindFloat("pan", &pan);
            if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_pan(t, pan);
            break;
        }
        case MSG_MIX_TOGGLE_MUTE: {
            int32 slot = -1;
            bool on = false;
            message->FindInt32("slot", &slot);
            message->FindBool("on", &on);
            if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_muted(t, on);
            break;
        }
        case MSG_MIX_TOGGLE_SOLO: {
            int32 slot = -1;
            bool on = false;
            message->FindInt32("slot", &slot);
            message->FindBool("on", &on);
            if (JackDawTrack *t = TrackForSlot(slot))
                jackdaw_track_set_soloed(t, on);
            break;
        }

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
    // Force the detached mixer window closed first (its QuitRequested normally
    // re-docks rather than quits, which would otherwise block app shutdown).
    if (m_mixer_window && m_mixer_window->Lock()) {
        m_mixer_window->Quit(); // deletes the window
        m_mixer_window = NULL;
    }
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}
