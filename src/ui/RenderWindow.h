#pragma once

#include <Messenger.h>
#include <Window.h>

#include "engine/project.h"

class BButton;
class BFilePanel;
class BMenuField;
class BStatusBar;
class BTextControl;

// Render/export options + progress dialog. This window runs its own looper, so
// it never touches the engine directly: the render drives the engine's non-RT
// API (suspend, locate, transport, master tap), which belongs to the MAIN
// window's looper. The dialog only gathers options and posts MSG_RENDER_START /
// MSG_RENDER_CANCEL to the main window; the main window owns the render
// lifecycle + poll tick and posts MSG_RENDER_PROGRESS back here for display.
//
// It is a persistent singleton (hidden on close, force-quit by the main window
// on app shutdown) so it can keep showing progress while a render runs.
class RenderWindow : public BWindow
{
public:
    RenderWindow(JackDawProject *project, BMessenger main);
    ~RenderWindow() override;

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override; // hide, don't destroy (main force-quits on exit)

    // Present the dialog, preselecting whole-project or loop-region scope.
    void Present(bool region_scope);

private:
    void RebuildFormatState();  // enable/disable bit-depth + FLAC/MP3 by support
    void SyncOutputExtension(); // make the out-path extension match the format
    void StartRender();         // validate + post MSG_RENDER_START
    void SetRunning(bool running);

    int SelectedFormat() const;   // RenderFormat
    int SelectedBitDepth() const; // RenderBitDepth
    int SelectedSource() const;   // RenderSource
    int SelectedScope() const;    // RenderScope
    int SelectedMethod() const;   // RenderMethod
    int SelectedSampleRate() const;
    int SelectedChannels() const;

    JackDawProject *m_project; // borrowed
    BMessenger m_main;

    BMenuField *m_format;
    BMenuField *m_bits;
    BMenuField *m_source;
    BMenuField *m_scope;
    BMenuField *m_rate;
    BMenuField *m_channels;
    BMenuField *m_method;
    BTextControl *m_out;
    BButton *m_browse;
    BButton *m_render;
    BButton *m_close;
    BStatusBar *m_status;

    BFilePanel *m_save_panel; // "Browse" for the output file (lazy)
    bool m_running;
};
