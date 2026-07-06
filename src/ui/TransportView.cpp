#include "TransportView.h"

#include <Button.h>
#include <CheckBox.h>
#include <Font.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <stdio.h>
#include <stdlib.h>

#include "engine/jackdaw-engine.h"
#include "engine/tempomap.h"
#include "engine/timeruler.h"
#include "Messages.h"

TransportView::TransportView(JackDawProject *project)
    : BView("transport", B_WILL_DRAW), m_project(project)
{
    m_rtz_button = new BButton("|◀", new BMessage(MSG_TRANSPORT_RTZ));
    m_play_button = new BButton("▶", new BMessage(MSG_TRANSPORT_PLAY));
    m_stop_button = new BButton("■", new BMessage(MSG_TRANSPORT_STOP));
    m_record_button = new BButton("●", new BMessage(MSG_TRANSPORT_RECORD));

    m_readout = new BStringView("readout", "00:00.000");
    m_state = new BStringView("state", "");
    BFont font(be_bold_font);
    font.SetSize(font.Size() * 1.6f);
    m_readout->SetFont(&font);
    m_state->SetFont(&font);
    m_readout->SetAlignment(B_ALIGN_LEFT);
    m_state->SetAlignment(B_ALIGN_LEFT);

    m_bpm = new BTextControl("BPM", "120", new BMessage(MSG_SET_BPM));
    m_sig_num = new BTextControl("", "4", new BMessage(MSG_SET_TIMESIG));
    m_sig_den = new BTextControl("/", "4", new BMessage(MSG_SET_TIMESIG));

    m_metronome = new BCheckBox("Click", new BMessage(MSG_TOGGLE_METRONOME));
    m_grid = new BCheckBox("Grid", new BMessage(MSG_TOGGLE_GRID));
    m_snap = new BCheckBox("Snap", new BMessage(MSG_TOGGLE_SNAP));
    m_bars_mode = new BCheckBox("Bars", new BMessage(MSG_TOGGLE_RULER_MODE));

    BLayoutBuilder::Group<>(this, B_HORIZONTAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS)
        .Add(m_rtz_button)
        .Add(m_play_button)
        .Add(m_stop_button)
        .Add(m_record_button)
        .AddStrut(12.0f)
        .Add(m_readout)
        .Add(m_state)
        .AddGlue()
        .Add(m_bpm)
        .Add(m_sig_num)
        .Add(m_sig_den)
        .AddStrut(12.0f)
        .Add(m_metronome)
        .Add(m_grid)
        .Add(m_snap)
        .Add(m_bars_mode);
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

    // Tempo/toggle controls apply straight to the project on this window's
    // looper thread; the transport buttons keep their default target (the
    // window), which owns all engine transport calls.
    m_bpm->SetTarget(this);
    m_sig_num->SetTarget(this);
    m_sig_den->SetTarget(this);
    m_metronome->SetTarget(this);
    m_grid->SetTarget(this);
    m_snap->SetTarget(this);
    m_bars_mode->SetTarget(this);

    SyncControls();
    UpdateReadout();
}

void TransportView::SyncControls()
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", jackdaw_project_get_bpm(m_project));
    m_bpm->SetText(buf);
    snprintf(buf, sizeof(buf), "%u", m_project->beats_per_bar);
    m_sig_num->SetText(buf);
    snprintf(buf, sizeof(buf), "%u", m_project->beat_unit);
    m_sig_den->SetText(buf);
    m_metronome->SetValue(m_project->metronome_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
    m_grid->SetValue(m_project->grid_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
    m_snap->SetValue(m_project->snap_enabled ? B_CONTROL_ON : B_CONTROL_OFF);
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
        format_timecode(sr, pos, 0, tc, TIMEMODE_REALLONG);
        snprintf(text, sizeof(text), "%s", tc);
    }

    m_readout->SetText(text);

    if (jackdaw_engine_is_counting_in())
        m_state->SetText("…");
    else if (jackdaw_engine_is_recording())
        m_state->SetText("●");
    else if (jackdaw_engine_is_playing())
        m_state->SetText("▶");
    else
        m_state->SetText("");
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
            double bpm = atof(m_bpm->Text());
            if (bpm > 0.0)
                jackdaw_project_set_bpm(m_project, bpm);
            SyncControls(); /* show the clamped value */
            /* Release keyboard focus so Space works as transport toggle again
             * (the key filter leaves text fields alone while one is focused).
             * Also clear the selection: the control's Enter handling runs
             * SelectAll() after posting this message, which otherwise leaves
             * the number highlighted as if still focused. */
            m_bpm->MakeFocus(false);
            m_bpm->TextView()->Select(0, 0);
            break;
        }
        case MSG_SET_TIMESIG: {
            int num = atoi(m_sig_num->Text());
            int den = atoi(m_sig_den->Text());
            if (num > 0 && den > 0)
                jackdaw_project_set_time_signature(m_project, (guint)num, (guint)den);
            SyncControls();
            m_sig_num->MakeFocus(false);
            m_sig_den->MakeFocus(false);
            m_sig_num->TextView()->Select(0, 0);
            m_sig_den->TextView()->Select(0, 0);
            break;
        }
        case MSG_TOGGLE_METRONOME:
            jackdaw_project_set_metronome(m_project, m_metronome->Value() == B_CONTROL_ON);
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
        default:
            BView::MessageReceived(message);
            break;
    }
}
