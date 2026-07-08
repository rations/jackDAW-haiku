#pragma once

#include <Messenger.h>
#include <Window.h>

class BSlider;
class BStringView;

// Modal "Region Gain" dialog: a dB slider (-25..+25). On Apply it posts
// MSG_REGION_SET_GAIN { float "db" } to the target (the TrackAreaView) and
// closes; Cancel/Esc just closes. Runs on its own looper and deletes itself on
// quit, so the caller only news + Show()s it. The single-mutator rule is kept:
// the dialog never touches the project, only posts the chosen value.
class RegionGainWindow : public BWindow
{
public:
    RegionGainWindow(BMessenger target, double seed_db);

    void MessageReceived(BMessage *message) override;

private:
    void UpdateLabel(int32 db);

    BMessenger m_target;
    BSlider *m_slider;
    BStringView *m_value_label;
};
