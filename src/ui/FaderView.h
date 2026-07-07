#pragma once

#include <Invoker.h>
#include <View.h>

// Vertical channel fader with a dB taper (-42 dB bottom .. +12 dB top; unity
// 0 dB sits ~78% up the travel, like a mixing-desk fader). Hand-drawn dB scale
// beside the slider, double-click returns to 0 dB, scroll steps it. On change
// it Invokes its message with a linear-gain float "gain" (= 10^(dB/20)); the
// owner writes that to the track fader or the master volume.
class FaderView : public BView, public BInvoker
{
public:
    FaderView(const char *name, float gain, BMessage *message);

    void AttachedToWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override;
    void MessageReceived(BMessage *message) override;

    // Set from a linear gain without notifying (sync from engine state).
    void SetGain(float gain);

private:
    void SetPosNotify(double pos); // clamp, redraw, Invoke
    double YToPos(float y) const;
    float PosToY(double pos) const;

    double m_pos; // 0 (bottom) .. 1 (top)
    bool m_dragging;
};
