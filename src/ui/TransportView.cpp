#include "TransportView.h"

#include <Button.h>
#include <CheckBox.h>
#include <Font.h>
#include <LayoutBuilder.h>
#include <Looper.h>
#include <Message.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>

#include "engine/jackdaw-engine.h"
#include "engine/tempomap.h"
#include "engine/timeruler.h"
#include "Messages.h"
#include "StateButton.h"
#include "StepperControl.h"

// Timecode formats cycled by clicking the position readout (time-ruler mode).
static const int32 kTimeModes[] = {TIMEMODE_REALLONG, TIMEMODE_REAL,  TIMEMODE_SAMPLES,
                                   TIMEMODE_24FPS,    TIMEMODE_25FPS, TIMEMODE_NTSC,
                                   TIMEMODE_30FPS};
static const int kNumTimeModes = (int)(sizeof(kTimeModes) / sizeof(kTimeModes[0]));

// A StateButton that, on a right-click, posts a context-menu trigger (with the
// screen point) to its window instead of invoking/toggling. Used for the
// Record, Metro and Mixer buttons. Deriving from StateButton lets Record carry
// the red active tint + ring/bullseye glyph while keeping the right-click menu.
class ContextButton : public StateButton
{
public:
    ContextButton(const char *name, const char *label, BMessage *invoke, uint32 context_what)
        : StateButton(name, label, invoke), m_context_what(context_what)
    {
    }

    void MouseDown(BPoint where) override
    {
        int32 buttons = 0;
        if (Looper()) {
            BMessage *cur = Looper()->CurrentMessage();
            if (cur)
                cur->FindInt32("buttons", &buttons);
        }
        if (buttons & B_SECONDARY_MOUSE_BUTTON) {
            ConvertToScreen(&where);
            BMessage msg(m_context_what);
            msg.AddPoint("screen_where", where);
            if (Window())
                Window()->PostMessage(&msg);
            return; // don't invoke/toggle on right-click
        }
        BButton::MouseDown(where);
    }

private:
    uint32 m_context_what;
};

// A BStringView that posts a message to a target handler when clicked — the
// position readout, which cycles the timecode format.
class ClickableStringView : public BStringView
{
public:
    ClickableStringView(const char *name, const char *text, uint32 click_what)
        : BStringView(name, text), m_click_what(click_what), m_target(NULL)
    {
    }

    void SetClickTarget(BHandler *target)
    {
        m_target = target;
    }

    void MouseDown(BPoint where) override
    {
        (void)where;
        if (m_target && Looper())
            Looper()->PostMessage(m_click_what, m_target);
    }

private:
    uint32 m_click_what;
    BHandler *m_target;
};

TransportView::TransportView(JackDawProject *project)
    : BView("transport", B_WILL_DRAW), m_project(project), m_timemode(TIMEMODE_REALLONG)
{
    m_rtz_button = new BButton("|◀", new BMessage(MSG_TRANSPORT_RTZ));
    m_step_back = new BButton("|<<", new BMessage(MSG_TRANSPORT_STEP_BACK));
    m_step_fwd = new BButton(">>|", new BMessage(MSG_TRANSPORT_STEP_FWD));
    m_next_button = new BButton("▶|", new BMessage(MSG_TRANSPORT_NEXT_BOUNDARY));
    // Loop and Record keep their glyph as a label for layout sizing only; the
    // glyph itself is vector-drawn (StateButton::GLYPH_*), so the label text is
    // never painted (the "⟳" tofu the default font renders never appears).
    m_play_button = new StateButton("play", "▶", new BMessage(MSG_TRANSPORT_PLAY));
    m_loop_button = new StateButton("loop", "⟳", new BMessage(MSG_TRANSPORT_LOOP));
    m_pause_button = new BButton("||", new BMessage(MSG_TRANSPORT_PAUSE));
    m_stop_button = new BButton("■", new BMessage(MSG_TRANSPORT_STOP));
    m_record_button =
        new ContextButton("record", "●", new BMessage(MSG_TRANSPORT_RECORD), MSG_RECORD_MENU);

    m_play_button->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_loop_button->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_record_button->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);

    // State colours (match the original: Play green, Loop light-green with a
    // drawn loop arrow so it never depends on a font glyph, Record red with a
    // ring/bullseye glyph that differs between normal and punch modes).
    m_play_button->SetActiveColor((rgb_color){46, 139, 87, 255}, (rgb_color){255, 255, 255, 255});
    m_loop_button->SetGlyph(StateButton::GLYPH_LOOP);
    m_loop_button->SetActiveColor((rgb_color){140, 230, 140, 255}, (rgb_color){16, 16, 16, 255});
    m_record_button->SetGlyph(StateButton::GLYPH_RECORD);
    m_record_button->SetActiveColor((rgb_color){192, 57, 43, 255}, (rgb_color){255, 255, 255, 255});

    ClickableStringView *readout =
        new ClickableStringView("readout", "00:00.000", MSG_CYCLE_TIMEMODE);
    m_readout = readout;
    m_state = new BStringView("state", "");
    BFont font(be_bold_font);
    font.SetSize(font.Size() * 1.6f);
    m_readout->SetFont(&font);
    m_state->SetFont(&font);
    m_readout->SetAlignment(B_ALIGN_LEFT);
    m_state->SetAlignment(B_ALIGN_LEFT);

    m_split_button = new BButton("Split", new BMessage(MSG_SPLIT));
    m_grid = new BCheckBox("Grid", new BMessage(MSG_TOGGLE_GRID));
    m_snap = new BCheckBox("Snap", new BMessage(MSG_TOGGLE_SNAP));
    m_metro_button =
        new ContextButton("metro", "Metro", new BMessage(MSG_TOGGLE_METRONOME), MSG_METRO_MENU);
    m_metro_button->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_bars_mode = new BCheckBox("Bars", new BMessage(MSG_TOGGLE_RULER_MODE));

    m_bpm = new StepperControl("bpm", "BPM", new BMessage(MSG_SET_BPM), 20, 999, 1, 0);
    m_sig_num = new StepperControl("signum", "Sig", new BMessage(MSG_SET_TIMESIG), 1, 32, 1, 0);
    m_sig_den = new StepperControl("sigden", "/", new BMessage(MSG_SET_TIMESIG), 1, 32, 1, 0);

    m_mixer_button =
        new ContextButton("mixer", "Mixer", new BMessage(MSG_MIXER_TOGGLE), MSG_MIXER_MENU);
    m_mixer_button->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS, B_USE_SMALL_INSETS, B_USE_SMALL_INSETS, 0)
        .Add(m_rtz_button)
        .Add(m_step_back)
        .Add(m_step_fwd)
        .Add(m_next_button)
        .Add(new BSeparatorView(B_VERTICAL))
        .Add(m_play_button)
        .Add(m_loop_button)
        .Add(m_pause_button)
        .Add(m_stop_button)
        .Add(m_record_button)
        .AddStrut(12.0f)
        .Add(m_readout)
        .Add(m_state)
        .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS, 0, B_USE_SMALL_INSETS, B_USE_SMALL_INSETS)
        .Add(m_split_button)
        .Add(m_metro_button)
        .Add(new BSeparatorView(B_VERTICAL))
        .Add(m_grid)
        .Add(m_snap)
        .Add(m_bars_mode)
        .AddStrut(12.0f)
        .Add(m_bpm)
        .Add(m_sig_num)
        .Add(m_sig_den)
        .AddGlue()
        .Add(m_mixer_button)
        .End();
}

void TransportView::AttachedToWindow()
{
    BView::AttachedToWindow();
    SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

    // Pin the readout and state indicator to fixed widths (widest expected
    // strings) so the transport bar never re-flows when playback state or the
    // position format changes.
    float readout_w = m_readout->StringWidth("00:00:00.000");
    float w = m_readout->StringWidth("9999.99.9999");
    if (w > readout_w)
        readout_w = w;
    readout_w += 8.0f;
    m_readout->SetExplicitMinSize(BSize(readout_w, B_SIZE_UNSET));
    m_readout->SetExplicitMaxSize(BSize(readout_w, B_SIZE_UNSET));

    float state_w = m_state->StringWidth("●") + 8.0f;
    m_state->SetExplicitMinSize(BSize(state_w, B_SIZE_UNSET));
    m_state->SetExplicitMaxSize(BSize(state_w, B_SIZE_UNSET));

    // Project-scoped controls apply on this looper; the readout cycles its own
    // timecode format. The transport/loop/mixer buttons keep their default
    // target (the window), which owns the engine calls.
    static_cast<ClickableStringView *>(m_readout)->SetClickTarget(this);
    m_metro_button->SetTarget(this);
    m_grid->SetTarget(this);
    m_snap->SetTarget(this);
    m_bars_mode->SetTarget(this);
    m_bpm->SetTarget(this);
    m_sig_num->SetTarget(this);
    m_sig_den->SetTarget(this);

    SyncControls();
    UpdateReadout();
}

void TransportView::SyncControls()
{
    m_bpm->SetValue(jackdaw_project_get_bpm(m_project));
    m_sig_num->SetValue(m_project->beats_per_bar);
    m_sig_den->SetValue(m_project->beat_unit);
    m_grid->SetValue(m_project->grid_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
    m_snap->SetValue(m_project->snap_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
    m_metro_button->SetValue(m_project->metronome_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
    m_bars_mode->SetValue(m_project->ruler_mode == JACKDAW_RULER_BARS ? B_CONTROL_ON
                                                                      : B_CONTROL_OFF);
}

void TransportView::UpdateReadout()
{
    off_t pos = jackdaw_engine_get_play_pos();
    guint32 sr = jackdaw_engine_get_sample_rate();
    char text[80];

    if (m_project->ruler_mode == JACKDAW_RULER_BARS) {
        TempoMap tm;
        tempomap_from_project(&tm, m_project, sr);
        TempoMapBBT bbt;
        tempomap_frame_to_bbt(&tm, pos, &bbt);
        snprintf(text, sizeof(text), "%u.%u.%03u", bbt.bar, bbt.beat, bbt.tick);
    } else {
        char tc[64];
        format_timecode(sr, pos, 0, tc, m_timemode);
        snprintf(text, sizeof(text), "%s", tc);
    }

    m_readout->SetText(text);

    bool playing = jackdaw_engine_is_playing();
    bool recording = jackdaw_engine_is_recording();
    bool counting = jackdaw_engine_is_counting_in();

    if (counting)
        m_state->SetText("…");
    else if (recording)
        m_state->SetText("●");
    else if (playing)
        m_state->SetText("▶");
    else
        m_state->SetText("");

    // Reflect engine state on the toggle buttons (state can change via keyboard,
    // menu, count-in hand-off or loop, not only the buttons themselves).
    m_play_button->SetValue((playing || counting) ? B_CONTROL_ON : B_CONTROL_OFF);
    m_record_button->SetValue(recording ? B_CONTROL_ON : B_CONTROL_OFF);
    m_record_button->SetRecordState(recording,
                                    jackdaw_engine_get_record_mode() == RECORD_MODE_PUNCH);
    m_loop_button->SetValue(jackdaw_engine_get_loop_enabled() ? B_CONTROL_ON : B_CONTROL_OFF);
}

void TransportView::MouseDown(BPoint where)
{
    MakeFocus(true); /* pull focus off a BPM/timesig text control */
    BView::MouseDown(where);
}

void TransportView::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_SET_BPM: {
            double bpm = m_bpm->Value();
            if (bpm > 0.0)
                jackdaw_project_set_bpm(m_project, bpm);
            m_bpm->SetValue(jackdaw_project_get_bpm(m_project)); /* show clamp */
            break;
        }
        case MSG_SET_TIMESIG: {
            int num = (int)m_sig_num->Value();
            int den = (int)m_sig_den->Value();
            if (num > 0 && den > 0)
                jackdaw_project_set_time_signature(m_project, (guint)num, (guint)den);
            m_sig_num->SetValue(m_project->beats_per_bar);
            m_sig_den->SetValue(m_project->beat_unit);
            break;
        }
        case MSG_TOGGLE_METRONOME:
            jackdaw_project_set_metronome(m_project, m_metro_button->Value() == B_CONTROL_ON);
            break;
        case MSG_TOGGLE_GRID:
            jackdaw_project_set_grid_enabled(m_project, m_grid->Value() == B_CONTROL_ON);
            break;
        case MSG_TOGGLE_SNAP:
            jackdaw_project_set_snap_enabled(m_project, m_snap->Value() == B_CONTROL_ON);
            break;
        case MSG_TOGGLE_RULER_MODE:
            jackdaw_project_set_ruler_mode(m_project, m_bars_mode->Value() == B_CONTROL_ON
                                                          ? JACKDAW_RULER_BARS
                                                          : JACKDAW_RULER_TIME);
            UpdateReadout();
            break;
        case MSG_CYCLE_TIMEMODE: {
            int idx = 0;
            for (int i = 0; i < kNumTimeModes; i++)
                if (kTimeModes[i] == m_timemode)
                    idx = i;
            m_timemode = kTimeModes[(idx + 1) % kNumTimeModes];
            UpdateReadout();
            break;
        }
        default:
            BView::MessageReceived(message);
            break;
    }
}
