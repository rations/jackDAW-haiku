#include "MainWindow.h"

#include <Application.h>

#include "Messages.h"

MainWindow::MainWindow()
    : BWindow(BRect(100, 100, 1100, 700), "JackDAW", B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS)
{
}

void MainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool MainWindow::QuitRequested()
{
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}
