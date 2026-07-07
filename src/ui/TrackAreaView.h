#pragma once

#include <Messenger.h>
#include <View.h>

#include <vector>

#include "engine/project.h"

class TimelineView;
class TrackStripView;

// The area below the ruler: a vertical stack of per-track rows. Each row is a
// TrackStripView (left, in the 235 px strip column) plus a timeline lane drawn
// by this view (right of the column). Empty space beneath the last row keeps
// the click-to-locate behaviour. Strips are real child BViews (interactive
// controls); lanes have no widgets yet (clips arrive in a later phase) so this
// view draws them directly. Vertical scrolling is a pixel offset driven by the
// timeline's vertical scrollbar. Runs on the MainWindow looper thread.
class TrackAreaView : public BView
{
public:
    explicit TrackAreaView(TimelineView *timeline);
    ~TrackAreaView() override;

    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override;
    void MessageReceived(BMessage *message) override;
    void FrameResized(float w, float h) override;

    // Rebuild the strip children from the project's current track list.
    void RebuildStrips();
    // Total pixel height of all rows (for the vertical scrollbar range).
    float ContentHeight() const;
    void SetScrollY(float y);
    float ScrollY() const
    {
        return m_scroll_y;
    }
    // Push fresh peak values into each strip's VU (called on the UI tick).
    void UpdateMeters();

private:
    void RepositionStrips();
    int RowAtY(float y) const; // track index under a lane y, or -1

    TimelineView *m_timeline; // borrowed (parent)
    BMessenger m_main;        // MainWindow (for strip context/structural msgs)
    std::vector<TrackStripView *> m_strips;

    float m_scroll_y;
    bool m_dragging;

    gulong m_added_handler;
    gulong m_removed_handler;
    gulong m_reordered_handler;
    gulong m_selection_handler;
};
