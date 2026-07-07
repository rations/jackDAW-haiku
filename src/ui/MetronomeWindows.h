#pragma once

#include <Messenger.h>
#include <Window.h>

class BSlider;
class BStringView;
class StepperControl;

// Metronome option windows opened from the Metro button's right-click menu.
// Each runs its own looper thread, so per the single-mutator rule it never
// touches the project directly: it posts value messages to the MainWindow
// messenger, which applies them. Closing hides the window (MainWindow keeps the
// singleton and re-syncs it on the next open).

// Volume: a dB slider (-25..+25 dB). Posts MSG_METRO_SET_VOLUME { float db }.
class MetroVolumeWindow : public BWindow
{
public:
    explicit MetroVolumeWindow(BMessenger target);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override; // hide, keep the singleton alive

    void SyncVolume(double db); // re-seed the slider on (re)open

private:
    void UpdateLabel(int32 db);

    BMessenger m_target;
    BSlider *m_slider;
    BStringView *m_value_label;
};

// Count-in: metronome pre-roll before record / before play (0..32 beats).
// Posts MSG_METRO_SET_COUNTIN_REC / _PLAY { int32 beats }.
class CountInWindow : public BWindow
{
public:
    explicit CountInWindow(BMessenger target);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    void SyncCountin(int rec_beats, int play_beats);

private:
    BMessenger m_target;
    StepperControl *m_rec;
    StepperControl *m_play;
};
