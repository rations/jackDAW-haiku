#include "IoWindow.h"

#include <LayoutBuilder.h>
#include <StringView.h>

#include "Messages.h"
#include "StepperControl.h"
#include "engine/track.h" // JACKDAW_MAX_TRACKS (port-count ceiling)

// Internal message: either stepper changed; the handler re-reads both.
enum {
    MSG_IO_CHANGED = 'iolc',
};

IoWindow::IoWindow(BMessenger target)
    : BWindow(BRect(150, 150, 500, 260), "Inputs / Outputs", B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
      m_target(target), m_inputs(NULL), m_outputs(NULL)
{
    m_inputs = new StepperControl("inputs", "Audio inputs:", new BMessage(MSG_IO_CHANGED), 1,
                                  JACKDAW_MAX_TRACKS, 1, 0);
    m_outputs = new StepperControl("outputs", "Audio outputs:", new BMessage(MSG_IO_CHANGED), 1,
                                   JACKDAW_MAX_TRACKS, 1, 0);

    BStringView *hint =
        new BStringView("hint", "JACK capture (in_N) and master output (out_N) port counts.");

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(m_inputs)
        .Add(m_outputs)
        .Add(hint);

    // A StepperControl is a BView+BInvoker (not a BControl), so it does not
    // auto-target its window; route both steppers here.
    m_inputs->SetTarget(this);
    m_outputs->SetTarget(this);
}

void IoWindow::SyncCounts(int inputs, int outputs)
{
    m_inputs->SetValue(inputs); // clamps + reformats, does not Invoke
    m_outputs->SetValue(outputs);
}

void IoWindow::PostCounts()
{
    BMessage out(MSG_OPT_IO_APPLY);
    out.AddInt32("inputs", (int32)(m_inputs->Value() + 0.5));
    out.AddInt32("outputs", (int32)(m_outputs->Value() + 0.5));
    m_target.SendMessage(&out);
}

void IoWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_IO_CHANGED:
            PostCounts();
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool IoWindow::QuitRequested()
{
    Hide();
    return false; // keep the singleton alive; MainWindow re-shows it
}
