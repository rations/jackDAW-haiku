#include "RegionGainWindow.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <Slider.h>
#include <StringView.h>

#include <math.h>
#include <stdio.h>

#include "Messages.h"

enum {
    MSG_GAIN_SLIDER = 'gsld',
    MSG_GAIN_APPLY = 'gapl',
    MSG_GAIN_CANCEL = 'gcnl',
};

static const int32 kGainDbMin = -25;
static const int32 kGainDbMax = 25;

RegionGainWindow::RegionGainWindow(BMessenger target, double seed_db)
    : BWindow(BRect(0, 0, 320, 130), "Region Gain", B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
      m_target(target)
{
    if (seed_db < (double)kGainDbMin)
        seed_db = (double)kGainDbMin;
    if (seed_db > (double)kGainDbMax)
        seed_db = (double)kGainDbMax;
    int32 seed = (int32)lround(seed_db);

    m_slider =
        new BSlider("gain", "Gain (dB) for the selected area:", new BMessage(MSG_GAIN_SLIDER),
                    kGainDbMin, kGainDbMax, B_HORIZONTAL);
    m_slider->SetModificationMessage(new BMessage(MSG_GAIN_SLIDER));
    m_slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
    m_slider->SetHashMarkCount(11);
    m_slider->SetValue(seed);

    m_value_label = new BStringView("val", "");
    UpdateLabel(seed);

    BButton *apply = new BButton("apply", "Apply", new BMessage(MSG_GAIN_APPLY));
    BButton *cancel = new BButton("cancel", "Cancel", new BMessage(MSG_GAIN_CANCEL));

    // clang-format off
    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(m_slider)
        .Add(m_value_label)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(apply)
        .End();
    // clang-format on

    apply->MakeDefault(true);
    CenterOnScreen();
}

void RegionGainWindow::UpdateLabel(int32 db)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%+d dB", (int)db);
    m_value_label->SetText(buf);
}

void RegionGainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_GAIN_SLIDER:
            UpdateLabel(m_slider->Value());
            break;
        case MSG_GAIN_APPLY: {
            BMessage out(MSG_REGION_SET_GAIN);
            out.AddFloat("db", (float)m_slider->Value());
            m_target.SendMessage(&out);
            Quit(); // deletes this window
            break;
        }
        case MSG_GAIN_CANCEL:
            Quit();
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}
