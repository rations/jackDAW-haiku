#pragma once

#include <Messenger.h>
#include <Window.h>

class StepperControl;

// Options -> Inputs/Outputs: sets how many JACK audio capture (in_N) and master
// output (out_N) ports JackDAW exposes. Runs its own looper thread, so per the
// single-mutator rule it never touches the engine directly: it posts the two
// counts to the MainWindow messenger, which applies them. Closing hides the
// window (MainWindow keeps the singleton and re-syncs it on the next open).
//
// MIDI ports are deliberately absent: instrument tracks register their own MIDI
// in/out ports on demand, so there is no fixed MIDI pool to resize here.
class IoWindow : public BWindow
{
public:
    explicit IoWindow(BMessenger target);

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override; // hide, keep the singleton alive

    void SyncCounts(int inputs, int outputs); // re-seed the steppers on (re)open

private:
    void PostCounts(); // send both current stepper values to MainWindow

    BMessenger m_target;
    StepperControl *m_inputs;
    StepperControl *m_outputs;
};
