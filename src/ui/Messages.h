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
    MSG_TRANSPORT_TOGGLE = 'plst',        // space bar: play <-> stop
    MSG_TRANSPORT_PAUSE = 'paus',         // ||: stop playback/recording, keep position
    MSG_TRANSPORT_STEP_BACK = 'stpb',     // |<< : nudge cursor back 10 ms (Left)
    MSG_TRANSPORT_STEP_FWD = 'stpf',      // >>| : nudge cursor forward 10 ms (Right)
    MSG_TRANSPORT_NEXT_BOUNDARY = 'nxtb', // ▶| : jump to next clip boundary
    MSG_TRANSPORT_LOOP = 'loop',          // ⟳ : toggle loop region

    // Right-click context-menu triggers, posted by transport buttons with a
    // BPoint "screen_where"; MainWindow builds and runs the popup there.
    MSG_RECORD_MENU = 'rmnu',
    MSG_METRO_MENU = 'mmnu',
    MSG_MIXER_MENU = 'xmnu',

    // Record-mode right-click menu (record button).
    MSG_RECORD_MODE_NORMAL = 'rmnr',
    MSG_RECORD_MODE_PUNCH = 'rmpn',

    // Timeline zoom (menu / keyboard).
    MSG_ZOOM_IN = 'zmin',
    MSG_ZOOM_OUT = 'zmot',

    // Region editing.
    MSG_SPLIT = 'splt', // split focused track at the cursor

    // Tempo / grid / display controls (handled by TransportView).
    MSG_SET_BPM = 'sbpm',
    MSG_SET_TIMESIG = 'tsig',
    MSG_TOGGLE_METRONOME = 'metr',
    MSG_TOGGLE_GRID = 'grid',
    MSG_TOGGLE_SNAP = 'snap',
    MSG_TOGGLE_RULER_MODE = 'rulm',
    MSG_CYCLE_TIMEMODE = 'tcyc', // click position readout: cycle timecode format

    // Metronome options (Metro button right-click popup).
    MSG_METRO_VOLUME = 'mtvl',     // open Volume window
    MSG_METRO_COUNTIN = 'mtci',    // open Count-in window
    MSG_METRO_HEADPHONES = 'mthp', // toggle "headphones only" route

    // Value changes posted from the (separate-looper) metronome option windows
    // to MainWindow, which applies them on its looper (single-mutator rule).
    MSG_METRO_SET_VOLUME = 'mtsv',       // float  "db"
    MSG_METRO_SET_COUNTIN_REC = 'mtcr',  // int32  "beats"
    MSG_METRO_SET_COUNTIN_PLAY = 'mtcp', // int32 "beats"

    // Mixer toggle (dock) + right-click "Open in Window".
    MSG_MIXER_TOGGLE = 'mixt',
    MSG_MIXER_OPEN_WINDOW = 'mixw',

    // Menu bar — File.
    MSG_FILE_OPEN = 'fopn',
    MSG_FILE_SAVE = 'fsav',
    MSG_FILE_SAVE_AS = 'fsas',
    MSG_FILE_RENDER = 'frnd',
    MSG_FILE_RENDER_REGION = 'frrg',
    MSG_FILE_NEW = 'fnew',
    // (Quit uses B_QUIT_REQUESTED.)

    // Menu bar — Edit.
    MSG_EDIT_UNDO = 'undo',
    MSG_EDIT_REDO = 'redo',

    // Menu bar — Track.
    MSG_TRACK_ADD = 'tadd',
    MSG_TRACK_ADD_MIDI = 'tadm',
    MSG_TRACK_LOAD_FILE = 'tldf',
    MSG_TRACK_DELETE = 'tdel',

    // Menu bar — Options.
    MSG_OPT_IO = 'oint',
    MSG_OPT_PLUGINS = 'oplg',
    MSG_OPT_MIDI_CONTROL = 'omid',

    // Engine event hook -> window (posted from JACK notification threads via
    // BMessenger; handlers re-read engine state, payloads carry no pointers).
    MSG_ENGINE_PORTS_CHANGED = 'eprt',
    MSG_ENGINE_CONNECTIONS_CHANGED = 'econ',
    MSG_ENGINE_SHUTDOWN = 'edwn',
};
