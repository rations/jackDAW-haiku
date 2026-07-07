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

    // Drop-insertion indicator for the track drag-reorder gesture. gap is the
    // boundary index in [0, track count] where the dragged row would land, or -1
    // to clear the indicator. Called by a TrackStripView mid-drag.
    void SetDropGap(int gap);

private:
    void RepositionStrips();
    int RowAtY(float y) const; // track index under a lane y, or -1
    // Draw every track's clip-region waveforms + region boundaries into the lane.
    void DrawWaveforms(BRect lane);

    TimelineView *m_timeline; // borrowed (parent)
    BMessenger m_main;        // MainWindow (for strip context/structural msgs)
    std::vector<TrackStripView *> m_strips;

    float m_scroll_y;
    bool m_dragging;
    int m_drop_gap; // reorder insertion boundary, or -1 when not dragging

    // Per-track "state-changed" handlers (region edits) → repaint the lane.
    // Parallel to m_strips; reconnected on every RebuildStrips().
    std::vector<gulong> m_track_state_handlers;

    gulong m_added_handler;
    gulong m_removed_handler;
    gulong m_reordered_handler;
    gulong m_selection_handler;
};
