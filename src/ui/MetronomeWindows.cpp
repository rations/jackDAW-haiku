#include "MetronomeWindows.h"

#include <LayoutBuilder.h>
#include <Slider.h>
#include <StringView.h>

#include <stdio.h>

#include "Messages.h"
#include "StepperControl.h"

// Metronome volume range, in whole dB, matching the project's clamp.
static const int32 kMetroDbMin = -25;
static const int32 kMetroDbMax = 25;

// Internal message: the slider targets the window.
enum {
    MSG_VOL_SLIDER = 'vsld',
};

MetroVolumeWindow::MetroVolumeWindow(BMessenger target)
    : BWindow(BRect(120, 120, 460, 220), "Metronome Volume", B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
      m_target(target), m_slider(NULL), m_value_label(NULL)
{
    m_slider = new BSlider("vol", "Metronome click volume", new BMessage(MSG_VOL_SLIDER),
                           kMetroDbMin, kMetroDbMax, B_HORIZONTAL);
    m_slider->SetLimitLabels("-25 dB", "+25 dB");
    m_slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
    m_slider->SetHashMarkCount(11);
    m_slider->SetModificationMessage(new BMessage(MSG_VOL_SLIDER));
    m_slider->SetTarget(this);

    m_value_label = new BStringView("value", "0 dB");

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(m_slider)
        .Add(m_value_label);
}

void MetroVolumeWindow::UpdateLabel(int32 db)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%+d dB", (int)db);
    m_value_label->SetText(buf);
}

void MetroVolumeWindow::SyncVolume(double db)
{
    int32 v = (int32)(db < 0 ? db - 0.5 : db + 0.5);
    if (v < kMetroDbMin)
        v = kMetroDbMin;
    if (v > kMetroDbMax)
        v = kMetroDbMax;
    m_slider->SetValue(v);
    UpdateLabel(v);
}

void MetroVolumeWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_VOL_SLIDER: {
            int32 db = m_slider->Value();
            UpdateLabel(db);
            BMessage out(MSG_METRO_SET_VOLUME);
            out.AddFloat("db", (float)db);
            m_target.SendMessage(&out);
            break;
        }
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MetroVolumeWindow::QuitRequested()
{
    Hide();
    return false; // keep the singleton alive; MainWindow re-shows it
}

// ---------------------------------------------------------------------------

CountInWindow::CountInWindow(BMessenger target)
    : BWindow(BRect(140, 140, 460, 240), "Count In", B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
      m_target(target), m_rec(NULL), m_play(NULL)
{
    m_rec = new StepperControl(
        "rec", "Count in before record:", new BMessage(MSG_METRO_SET_COUNTIN_REC), 0, 32, 1, 0);
    m_play = new StepperControl(
        "play", "Count in before playback:", new BMessage(MSG_METRO_SET_COUNTIN_PLAY), 0, 32, 1, 0);

    BStringView *hint =
        new BStringView("hint", "Metronome clicks before transport starts (0 = off).");

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(m_rec)
        .Add(m_play)
        .Add(hint);
}

void CountInWindow::SyncCountin(int rec_beats, int play_beats)
{
    m_rec->SetValue(rec_beats);
    m_play->SetValue(play_beats);
}

void CountInWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_METRO_SET_COUNTIN_REC:
        case MSG_METRO_SET_COUNTIN_PLAY: {
            // StepperControl attaches the new value as a float; forward it to
            // MainWindow as an integer beat count.
            float v = 0.0f;
            message->FindFloat("value", &v);
            BMessage out(message->what);
            out.AddInt32("beats", (int32)(v + 0.5f));
            m_target.SendMessage(&out);
            break;
        }
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool CountInWindow::QuitRequested()
{
    Hide();
    return false;
}
