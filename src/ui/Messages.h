#pragma once

// BMessage `what` constants shared by the JackDAW UI. One flat namespace so
// MainWindow::MessageReceived can route everything; grouped by source.
enum {
    // Periodic UI refresh (BMessageRunner owned by MainWindow).
    MSG_UI_TICK = 'tick',

    // Transport controls (buttons, menu items, keyboard shortcuts).
    MSG_TRANSPORT_PLAY = 'play',
    MSG_TRANSPORT_STOP = 'stop',
    MSG_TRANSPORT_RECORD = 'recd',
    MSG_TRANSPORT_RTZ = 'rtz ',

    // Engine event hook -> window (posted from JACK notification threads via
    // BMessenger; handlers re-read engine state, payloads carry no pointers).
    MSG_ENGINE_PORTS_CHANGED = 'eprt',
    MSG_ENGINE_CONNECTIONS_CHANGED = 'econ',
    MSG_ENGINE_SHUTDOWN = 'edwn',
};
