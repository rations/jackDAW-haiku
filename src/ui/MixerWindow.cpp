#include "MixerWindow.h"

#include <LayoutBuilder.h>
#include <MessageRunner.h>

#include "Messages.h"
#include "MixerView.h"

static const bigtime_t kTickIntervalUsec = 33000; // ~30 Hz

MixerWindow::MixerWindow(JackDawProject *project, const BMessenger &main)
    : BWindow(BRect(140, 480, 900, 740), "Mixer", B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE),
      m_main(main), m_mixer(NULL), m_tick(NULL)
{
    m_mixer = new MixerView(project, main);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0).Add(m_mixer);
    m_tick = new BMessageRunner(BMessenger(this), BMessage(MSG_MIXER_TICK), kTickIntervalUsec);
}

MixerWindow::~MixerWindow()
{
    delete m_tick;
}

void MixerWindow::Rebuild()
{
    if (m_mixer)
        m_mixer->Rebuild();
}

void MixerWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_MIXER_TICK:
            if (m_mixer) {
                m_mixer->Sync();
                m_mixer->UpdateMeters();
            }
            break;
        case MSG_MIXER_REBUILD:
            Rebuild();
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MixerWindow::QuitRequested()
{
    // Closing the detached mixer re-docks it: toggle the "in window" state back.
    m_main.SendMessage(MSG_MIXER_OPEN_WINDOW);
    return false; // MainWindow hides us; keep the object for reuse
}
