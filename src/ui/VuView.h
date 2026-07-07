#pragma once

#include <View.h>

// Stereo peak meter (green/yellow/red, -60 .. +6 dBFS). Fed linear peak values
// on the UI tick via SetPeaks(); applies a little UI-side decay so the fall-off
// reads smoothly regardless of how the source is sampled.
class VuView : public BView
{
public:
    explicit VuView(const char *name, float min_width = 14.0f);

    void AttachedToWindow() override;
    void Draw(BRect updateRect) override;

    // Linear peaks (0..1+). Mono sources pass the same value for both.
    void SetPeaks(float left, float right);

private:
    void DrawBar(BRect r, float disp);

    float m_left; // displayed (decayed) linear peaks
    float m_right;
    float m_min_width;
};
