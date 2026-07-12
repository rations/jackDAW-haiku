#ifndef FX_WINDOW_H
#define FX_WINDOW_H

#include <Window.h>

#include <vector>

#include "engine/track.h"

class BButton;
class BCheckBox;
class BFilePanel;
class BGroupView;
class BListView;
class BMenuField;
class BMessageRunner;
class BPopUpMenu;
class BSlider;
class BStringView;
class MainWindow;

typedef struct PluginInstance PluginInstance;

/* Per-track FX chain editor: add/remove/reorder plugins, a generic parameter
 * panel (one BSlider per VST3 parameter, values shown with the plug-in's own
 * display strings), bypass + wet/dry mix, and — for plug-ins implementing the
 * INamFileLoader host extension (NAMku) — "Load model…"/"Load IR…" buttons
 * backed by a BFilePanel.
 *
 * Threading: this window's looper thread is the ONLY mutator of its track's
 * FX chain and the only caller into each instance's non-RT plugin-host API
 * (instances are created and freed here too, satisfying the pluginhost.h
 * same-thread contract). The window holds a GObject ref on the track so the
 * chain outlives track removal while the window is open. */
class FxWindow : public BWindow
{
public:
    FxWindow(JackDawTrack *track, MainWindow *main);
    virtual ~FxWindow();

    virtual void MessageReceived(BMessage *message);

private:
    void BuildAddMenu();
    // Tell the main window the chain gained/lost a plug-in so the track + mixer
    // strips can refresh their blue Fx state.
    void NotifyChainChanged();
    void RebuildChainList(int select);
    void RebuildParamPanel();
    void BuildDrumRack(PluginInstance *inst);
    void ClearParamPanel();
    void SyncControlsRow();
    void UpdateValueLabel(guint param);
    void UpdateFileLabels();
    PluginInstance *Selected();

    JackDawTrack *m_track;
    MainWindow *m_main; // owner; borrowed (posts chain-changed, unregisters on close)

    BMenuField *m_add_field;
    BPopUpMenu *m_add_menu;
    BListView *m_chain_list;
    BButton *m_remove;
    BButton *m_up;
    BButton *m_down;
    BCheckBox *m_bypass;
    BSlider *m_mix;
    BGroupView *m_param_group;
    BFilePanel *m_file_panel;
    BFilePanel *m_save_panel; // B_SAVE_PANEL for writing preset files
    BButton *m_preset_save;
    BButton *m_preset_load;

    std::vector<BSlider *> m_sliders;
    std::vector<BStringView *> m_value_labels;
    BStringView *m_model_label;
    BStringView *m_ir_label;

    // Drum-rack rows (parallel, indexed by slot), plus MIDI-learn state.
    std::vector<BStringView *> m_drum_file;
    std::vector<BSlider *> m_drum_vol;
    std::vector<BCheckBox *> m_drum_learn;
    std::vector<BStringView *> m_drum_note;
    int m_learn_slot;             // slot whose Learn is armed, or -1
    BMessageRunner *m_learn_poll; // polls the RT-captured learn note
};

#endif // FX_WINDOW_H
