#pragma once

#include <View.h>

class TimelineView;

// The area below the ruler: a vertical stack of track rows (strip + lane),
// with empty timeline space beneath the last row. Phase-1 M3 state: no rows
// yet — draws the empty space (background, beat/bar grid when enabled, the
// playhead line) and locates on click, exactly what the space below the last
// track will keep doing once rows exist. The lane region starts at
// kTimelineHeaderWidth; the strip column is to its left.
class TrackAreaView : public BView
{
public:
    explicit TrackAreaView(TimelineView *timeline);

    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override;
    void MessageReceived(BMessage *message) override;

private:
    TimelineView *m_timeline; // borrowed (parent)
    bool m_dragging;
};
