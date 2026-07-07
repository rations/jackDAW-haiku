#include "TrackStripView.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <PopUpMenu.h>
#include <TextControl.h>
#include <Window.h>

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/jackdaw-engine.h"
#include "KnobView.h"
#include "Messages.h"
#include "VuView.h"

// GObject signal trampolines: a track's state/routing changed elsewhere (e.g.
// the mixer) — re-read it into this strip's controls. Emitted only on the
// MainWindow looper (the sole engine mutator), so touching views is safe.
static void strip_track_changed(gpointer /*obj*/, gpointer user)
{
    static_cast<TrackStripView *>(user)->SyncFromTrack();
}

// Internal messages (this strip is the target).
enum {
    MSG_S_NAME = 'snam',
    MSG_S_ARM = 'sarm',
    MSG_S_MUTE = 'smut',
    MSG_S_SOLO = 'ssol',
    MSG_S_STEREO = 'sste',
    MSG_S_VOL = 'svol',
    MSG_S_PAN = 'span',
    MSG_S_INPUT = 'sinp',
};

// Short JACK port name (drop the "client:" prefix) for menu labels.
static const char *port_short(const char *full)
{
    if (!full)
        return "";
    const char *colon = strrchr(full, ':');
    return (colon && colon[1]) ? colon + 1 : full;
}

TrackStripView::TrackStripView(JackDawProject *project, JackDawTrack *track, const BMessenger &main)
    : BView("track_strip", B_WILL_DRAW | B_FRAME_EVENTS), m_project(project), m_track(track),
      m_main(main), m_name(NULL), m_arm(NULL), m_mute(NULL), m_solo(NULL), m_stereo(NULL),
      m_fx(NULL), m_vol(NULL), m_pan(NULL), m_input_field(NULL), m_input_menu(NULL), m_vu(NULL),
      m_state_handler(0), m_routing_handler(0)
{
    m_name =
        new BTextControl("name", NULL, jackdaw_track_get_name(track), new BMessage(MSG_S_NAME));

    m_arm = new BButton("A", "A", new BMessage(MSG_S_ARM));
    m_mute = new BButton("M", "M", new BMessage(MSG_S_MUTE));
    m_solo = new BButton("S", "S", new BMessage(MSG_S_SOLO));
    m_stereo = new BButton("Mo", "Mo", new BMessage(MSG_S_STEREO));
    m_fx = new BButton("Fx", "Fx", new BMessage('sfx '));
    m_arm->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_mute->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_solo->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_stereo->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_fx->SetEnabled(false); // plugin hosting not yet available on Haiku
    m_fx->SetToolTip("Plugin hosting not yet available on Haiku");
    for (BButton *b : {m_arm, m_mute, m_solo, m_stereo, m_fx}) {
        b->SetExplicitMinSize(BSize(24.0f, 20.0f));
        b->SetExplicitMaxSize(BSize(30.0f, 20.0f));
    }

    double trim_db = 20.0 * log10((double)jackdaw_track_get_trim(track));
    if (!(trim_db > -25.0))
        trim_db = (jackdaw_track_get_trim(track) > 0.0001f) ? trim_db : 0.0;
    m_vol =
        new KnobView("V", -25.0, 25.0, trim_db, 0.0, KnobView::KIND_DB, new BMessage(MSG_S_VOL));
    m_vol->SetCenterLabel("V");
    m_pan = new KnobView("P", -1.0, 1.0, (double)jackdaw_track_get_pan(track), 0.0,
                         KnobView::KIND_PAN, new BMessage(MSG_S_PAN));
    m_pan->SetCenterLabel("P");

    m_input_menu = new BPopUpMenu("In");
    m_input_field = new BMenuField("input", NULL, m_input_menu);

    m_vu = new VuView("vu", 14.0f);

    // Left content column + a full-height VU pinned on the right. The content
    // stacks: name, the A/M/S/Mo/Fx button row, then the V/P knobs beside the
    // input selector.
    BLayoutBuilder::Group<>(this, B_HORIZONTAL, 3.0f)
        .SetInsets(4.0f, 3.0f, 3.0f, 3.0f)
        .AddGroup(B_VERTICAL, 2.0f)
        .Add(m_name)
        .AddGroup(B_HORIZONTAL, 2.0f)
        .Add(m_arm)
        .Add(m_mute)
        .Add(m_solo)
        .Add(m_stereo)
        .Add(m_fx)
        .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL, 3.0f)
        .Add(m_vol)
        .Add(m_pan)
        .Add(m_input_field)
        .End()
        .AddGlue()
        .End()
        .Add(m_vu)
        .End();
}

void TrackStripView::AttachedToWindow()
{
    BView::AttachedToWindow();
    // Opaque background so app_server erases the strip (and, by inheritance, its
    // custom child views) before each Draw. Selection is shown with an accent
    // bar rather than a full-strip tint, so the knobs keep matching the bg.
    SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

    m_name->SetTarget(this);
    m_arm->SetTarget(this);
    m_mute->SetTarget(this);
    m_solo->SetTarget(this);
    m_stereo->SetTarget(this);
    m_vol->SetTarget(this);
    m_pan->SetTarget(this);

    BuildInputMenu();
    SyncFromTrack();

    m_state_handler =
        g_signal_connect(m_track, "state-changed", G_CALLBACK(strip_track_changed), this);
    m_routing_handler =
        g_signal_connect(m_track, "routing-changed", G_CALLBACK(strip_track_changed), this);
}

void TrackStripView::DetachedFromWindow()
{
    if (m_state_handler) {
        g_signal_handler_disconnect(m_track, m_state_handler);
        m_state_handler = 0;
    }
    if (m_routing_handler) {
        g_signal_handler_disconnect(m_track, m_routing_handler);
        m_routing_handler = 0;
    }
    BView::DetachedFromWindow();
}

void TrackStripView::BuildInputMenu()
{
    // Rebuild the source list from the current physical JACK ports — audio
    // capture ports for audio tracks, MIDI capture ports for instrument tracks.
    while (m_input_menu->CountItems() > 0)
        delete m_input_menu->RemoveItem((int32)0);

    BMessage *none_msg = new BMessage(MSG_S_INPUT);
    none_msg->AddString("port", "");
    none_msg->AddInt32("index", -1);
    BMenuItem *none = new BMenuItem("None", none_msg);
    none->SetTarget(this);
    m_input_menu->AddItem(none);

    const char **ports = jackdaw_track_is_instrument(m_track) ? jackdaw_engine_list_midi_ports()
                                                              : jackdaw_engine_list_capture_ports();
    if (ports) {
        for (int i = 0; ports[i]; i++) {
            BMessage *m = new BMessage(MSG_S_INPUT);
            m->AddString("port", ports[i]);
            m->AddInt32("index", i);
            BMenuItem *it = new BMenuItem(port_short(ports[i]), m);
            it->SetTarget(this);
            m_input_menu->AddItem(it);
        }
        jackdaw_engine_free_ports(ports);
    }
}

void TrackStripView::SyncFromTrack()
{
    // Setting control values programmatically here does not re-invoke their
    // messages (BButton/BTextControl/KnobView::SetValue are all silent), so no
    // feedback guard is needed.
    if (strcmp(m_name->Text(), jackdaw_track_get_name(m_track)) != 0)
        m_name->SetText(jackdaw_track_get_name(m_track));
    m_arm->SetValue(jackdaw_track_is_armed(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
    m_mute->SetValue(jackdaw_track_is_muted(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
    m_solo->SetValue(jackdaw_track_is_soloed(m_track) ? B_CONTROL_ON : B_CONTROL_OFF);
    bool instrument = jackdaw_track_is_instrument(m_track);
    bool stereo = jackdaw_track_is_stereo(m_track);
    m_stereo->SetValue(stereo ? B_CONTROL_ON : B_CONTROL_OFF);
    m_stereo->SetLabel(instrument ? "MIDI" : (stereo ? "St" : "Mo"));
    m_stereo->SetEnabled(!instrument); // mono/stereo is an audio-track concept
    double trim_db = (jackdaw_track_get_trim(m_track) > 0.0001f)
                         ? 20.0 * log10((double)jackdaw_track_get_trim(m_track))
                         : -25.0;
    m_vol->SetValue(trim_db);
    m_pan->SetValue((double)jackdaw_track_get_pan(m_track));

    // Mark the current input source in the menu (MIDI or audio per track kind).
    const char *src = instrument ? m_track->midi_src_port : m_track->audio_src_port;
    for (int32 i = 0; i < m_input_menu->CountItems(); i++) {
        BMenuItem *it = m_input_menu->ItemAt(i);
        const char *port = "";
        it->Message()->FindString("port", &port);
        bool match = (!src && (!port || !*port)) || (src && port && strcmp(src, port) == 0);
        it->SetMarked(match);
    }
}

void TrackStripView::SetPeaks(float l, float r)
{
    if (m_vu)
        m_vu->SetPeaks(l, r);
}

void TrackStripView::ApplyStereo(bool stereo)
{
    jackdaw_engine_set_track_stereo(m_track, stereo);
    m_stereo->SetLabel(stereo ? "St" : "Mo");

    if (stereo) {
        // Auto-pair the right channel with the capture port following the
        // currently-selected left source, if one exists.
        const char *left = m_track->audio_src_port;
        if (left) {
            const char **ports = jackdaw_engine_list_capture_ports();
            if (ports) {
                for (int i = 0; ports[i]; i++) {
                    if (strcmp(ports[i], left) == 0 && ports[i + 1]) {
                        jackdaw_engine_set_audio_source_r(m_track, ports[i + 1]);
                        break;
                    }
                }
                jackdaw_engine_free_ports(ports);
            }
        }
    }
}

void TrackStripView::MouseDown(BPoint where)
{
    (void)where;
    int32 buttons = 0;
    int32 modifiers = 0;
    BMessage *msg = Window() ? Window()->CurrentMessage() : NULL;
    if (msg) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("modifiers", &modifiers);
    }

    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        BPoint screen = ConvertToScreen(where);
        BMessage m(MSG_TRACK_CONTEXT);
        m.AddInt32("slot", (int32)m_track->slot);
        m.AddPoint("screen_where", screen);
        m_main.SendMessage(&m);
        return;
    }

    // Left click selects the track (Shift/Cmd extends the selection).
    if (modifiers & (B_SHIFT_KEY | B_COMMAND_KEY))
        jackdaw_project_toggle_selected(m_project, m_track);
    else
        jackdaw_project_select_single(m_project, m_track);
}

void TrackStripView::Draw(BRect updateRect)
{
    (void)updateRect;
    BRect b = Bounds();
    // The opaque view colour has already erased the background. Just draw the
    // selection accent bar and the row dividers on top.
    if (jackdaw_project_is_selected(m_project, m_track)) {
        SetHighColor(60, 150, 230);
        FillRect(BRect(b.left, b.top, b.left + 2.0f, b.bottom));
    }
    SetHighColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT));
    StrokeLine(BPoint(b.right, b.top), BPoint(b.right, b.bottom));
    StrokeLine(BPoint(b.left, b.bottom), BPoint(b.right, b.bottom));
}

void TrackStripView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_S_NAME:
            jackdaw_track_set_name(m_track, m_name->Text());
            break;
        case MSG_S_ARM:
            jackdaw_track_set_armed(m_track, m_arm->Value() == B_CONTROL_ON);
            break;
        case MSG_S_MUTE:
            jackdaw_track_set_muted(m_track, m_mute->Value() == B_CONTROL_ON);
            break;
        case MSG_S_SOLO:
            jackdaw_track_set_soloed(m_track, m_solo->Value() == B_CONTROL_ON);
            break;
        case MSG_S_STEREO:
            ApplyStereo(m_stereo->Value() == B_CONTROL_ON);
            break;
        case MSG_S_VOL: {
            float db = 0.0f;
            message->FindFloat("value", &db);
            jackdaw_track_set_trim(m_track, (gfloat)pow(10.0, (double)db / 20.0));
            break;
        }
        case MSG_S_PAN: {
            float pan = 0.0f;
            message->FindFloat("value", &pan);
            jackdaw_track_set_pan(m_track, (gfloat)pan);
            break;
        }
        case MSG_S_INPUT: {
            const char *port = "";
            message->FindString("port", &port);
            const char *sel = (port && *port) ? port : NULL;
            if (jackdaw_track_is_instrument(m_track)) {
                jackdaw_engine_set_midi_source(m_track, sel);
            } else {
                jackdaw_engine_set_audio_source_l(m_track, sel);
                if (jackdaw_track_is_stereo(m_track))
                    ApplyStereo(true); // re-pair the right channel to the new source
            }
            SyncFromTrack();
            break;
        }
        default:
            BView::MessageReceived(message);
            break;
    }
}
