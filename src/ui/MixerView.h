#pragma once

#include <Messenger.h>
#include <View.h>

#include <vector>

#include "engine/project.h"

class BScrollBar;
class MixerStripView;

// The mixer: a single horizontally-scrolling row of fixed-width channel strips.
// The master strip is the first (leftmost) strip, then one strip per track, all
// the same fixed width — matching the Linux mixer's scrolled strip box. When the
// strips overflow the visible width a horizontal scrollbar appears; the strips
// never shrink to fit. Faders/VU expand to fill the available height.
//
// It holds no GObject signal handlers — its host (the main window when docked, or
// the mixer window when detached) drives Rebuild() on structural changes and
// Sync()/UpdateMeters() on the UI tick, so it is safe on either looper. Edits flow
// out through the strips to MainWindow.
class MixerView : public BView
{
public:
    MixerView(JackDawProject *project, const BMessenger &main);

    void AttachedToWindow() override;
    void FrameResized(float newWidth, float newHeight) override;

    void Rebuild();      // rebuild strips from the project's track list
    void Sync();         // reflect control state on all strips
    void UpdateMeters(); // push peaks into all VUs

    void SetScrollX(float x); // called by the horizontal scrollbar

private:
    void Relayout();         // position strips + scrollbar for the current bounds
    void UpdateHScrollBar(); // re-derive the scrollbar range from the strip count

    JackDawProject *m_project; // borrowed
    BMessenger m_main;
    BScrollBar *m_hscroll;
    std::vector<MixerStripView *> m_strips; // per-track (master is separate)
    MixerStripView *m_master;
    float m_scroll_x;
    bool m_built;
};
