#pragma once

#include <View.h>

class TimelineView;

// Time ruler above the track lanes. Draws tick marks + labels in the project's
// ruler mode (minutes:seconds via the ported tick ladder, or bars.beats from
// the tempo map) and the playhead marker. Click / drag = locate (snapped when
// snap is enabled). Local x 0 == lane x 0 (the strip column sits to our left).
class RulerView : public BView
{
public:
    explicit RulerView(TimelineView *timeline);

    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override;
    void MessageReceived(BMessage *message) override;

private:
    void DrawTimeTicks(BRect bounds);
    void DrawBarTicks(BRect bounds);
    void DrawLoop(BRect bounds);
    // Hit-test x against the loop tabs: 1 = start tab, 2 = end tab, 0 = none.
    int LoopHit(float x) const;
    void LoopDragTo(float x);

    TimelineView *m_timeline; // borrowed (parent)
    bool m_dragging;          // scrub drag in progress
    int m_loop_drag_edge;     // 0 none, 1 start tab, 2 end tab
};
