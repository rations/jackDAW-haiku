#pragma once

#include <Window.h>

class MainWindow : public BWindow
{
public:
    MainWindow();

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;
};
