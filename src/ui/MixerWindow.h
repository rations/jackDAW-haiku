#pragma once

#include <Messenger.h>
#include <Window.h>

#include "engine/project.h"

class BMessageRunner;
class MixerView;

// Detached mixer: a separate top-level window hosting its own MixerView. It runs
// on its own looper, so it never calls the engine directly — its strips post
// MSG_MIX_* to MainWindow (via the messenger passed in), and it refreshes on its
// own BMessageRunner tick. MainWindow forwards structural changes here with
// MSG_MIXER_REBUILD. Closing it asks MainWindow to re-dock (MSG_MIXER_OPEN_WINDOW).
class MixerWindow : public BWindow
{
public:
    MixerWindow(JackDawProject *project, const BMessenger &main);
    ~MixerWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    void Rebuild(); // rebuild strips (called on this looper)

private:
    BMessenger m_main;
    MixerView *m_mixer;
    BMessageRunner *m_tick;
};
