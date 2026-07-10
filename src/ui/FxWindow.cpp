#include "FxWindow.h"

#include <Button.h>
#include <CheckBox.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringItem.h>
#include <StringView.h>

#include <stdio.h>
#include <string.h>

#include "host/pluginhost.h"

enum {
    MSG_FX_ADD = 'fxad',    // string "key", "name", "category"
    MSG_FX_SELECT = 'fxsl', // chain list selection changed
    MSG_FX_REMOVE = 'fxrm',
    MSG_FX_UP = 'fxup',
    MSG_FX_DOWN = 'fxdn',
    MSG_FX_BYPASS = 'fxby',
    MSG_FX_MIX = 'fxmx',
    MSG_FX_PARAM = 'fxpm', // int32 "param" (index into the instance's params)
    MSG_FX_LOAD_MODEL = 'fxlm',
    MSG_FX_LOAD_IR = 'fxli',
    MSG_FX_FILE_REFS = 'fxrf', // BFilePanel result; int32 "which" from the template
    MSG_FX_RAISE = 'fxrs',     // re-activate an already-open window
};

static const int32 kSliderSteps = 1000; // BSlider is integer; params are [0,1]

FxWindow::FxWindow(JackDawTrack *track)
    : BWindow(BRect(0, 0, 560, 420), "Effects", B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
      m_track(track), m_file_panel(NULL), m_model_label(NULL), m_ir_label(NULL)
{
    g_object_ref(m_track);

    char title[128];
    snprintf(title, sizeof(title), "Effects — %s", jackdaw_track_get_name(m_track));
    SetTitle(title);

    m_add_menu = new BPopUpMenu("Add effect…");
    m_add_menu->SetRadioMode(false);
    m_add_field = new BMenuField("add", NULL, m_add_menu);

    m_chain_list = new BListView("chain");
    m_chain_list->SetSelectionMessage(new BMessage(MSG_FX_SELECT));
    BScrollView *chain_scroll =
        new BScrollView("chain-scroll", m_chain_list, 0, false, true, B_FANCY_BORDER);
    chain_scroll->SetExplicitMinSize(BSize(180.0f, 140.0f));

    m_remove = new BButton("remove", "Remove", new BMessage(MSG_FX_REMOVE));
    m_up = new BButton("up", "Up", new BMessage(MSG_FX_UP));
    m_down = new BButton("down", "Down", new BMessage(MSG_FX_DOWN));

    m_bypass = new BCheckBox("bypass", "Bypass", new BMessage(MSG_FX_BYPASS));
    m_mix = new BSlider("mix", "Mix (dry–wet)", new BMessage(MSG_FX_MIX), 0, kSliderSteps,
                        B_HORIZONTAL);
    m_mix->SetModificationMessage(new BMessage(MSG_FX_MIX));
    m_mix->SetValue(kSliderSteps);

    m_param_group = new BGroupView(B_VERTICAL, 4.0f);

    // clang-format off
    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL)
            .Add(m_add_field)
            .AddGlue()
        .End()
        .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
            .AddGroup(B_VERTICAL, 4.0f)
                .Add(chain_scroll)
                .AddGroup(B_HORIZONTAL, 4.0f)
                    .Add(m_remove)
                    .Add(m_up)
                    .Add(m_down)
                    .AddGlue()
                .End()
                .Add(m_bypass)
                .Add(m_mix)
                .AddGlue()
            .End()
            .Add(m_param_group)
        .End();
    // clang-format on

    BuildAddMenu();
    RebuildChainList(jackdaw_track_fx_count(m_track) > 0 ? 0 : -1);
    CenterOnScreen();
}

FxWindow::~FxWindow()
{
    delete m_file_panel;
    g_object_unref(m_track);
}

PluginInstance *FxWindow::Selected()
{
    int32 sel = m_chain_list->CurrentSelection();
    if (sel < 0)
        return NULL;
    return (PluginInstance *)jackdaw_track_fx_get(m_track, (guint)sel);
}

/* The catalog scan (out-of-process describe of every installed bundle) runs
 * lazily inside pluginhost_catalog() the first time any FX window opens. */
void FxWindow::BuildAddMenu()
{
    while (m_add_menu->CountItems() > 0)
        delete m_add_menu->RemoveItem((int32)0);

    const GList *cat = pluginhost_catalog();
    if (!cat) {
        BMenuItem *none = new BMenuItem("No plugins found", NULL);
        none->SetEnabled(false);
        m_add_menu->AddItem(none);
        return;
    }
    for (const GList *l = cat; l; l = l->next) {
        const PluginInfo *pi = (const PluginInfo *)l->data;
        BMessage *msg = new BMessage(MSG_FX_ADD);
        msg->AddString("key", pi->key);
        msg->AddString("name", pi->name);
        msg->AddString("category", pi->category);
        char label[160];
        snprintf(label, sizeof(label), "%s  (%s)", pi->name, pi->category);
        BMenuItem *item = new BMenuItem(label, msg);
        item->SetTarget(this);
        m_add_menu->AddItem(item);
    }
}

void FxWindow::RebuildChainList(int select)
{
    while (m_chain_list->CountItems() > 0)
        delete m_chain_list->RemoveItem((int32)0);

    guint n = jackdaw_track_fx_count(m_track);
    for (guint i = 0; i < n; i++) {
        PluginInstance *inst = (PluginInstance *)jackdaw_track_fx_get(m_track, i);
        char label[160];
        snprintf(label, sizeof(label), "%u. %s%s", i + 1, pluginhost_name(inst),
                 pluginhost_is_active(inst) ? "" : "  [bypassed]");
        m_chain_list->AddItem(new BStringItem(label));
    }
    if (select >= 0 && select < (int)n)
        m_chain_list->Select(select); // fires MSG_FX_SELECT → panel rebuild
    else
        RebuildParamPanel();
}

void FxWindow::SyncControlsRow()
{
    PluginInstance *inst = Selected();
    m_bypass->SetEnabled(inst != NULL);
    m_mix->SetEnabled(inst != NULL);
    m_remove->SetEnabled(inst != NULL);
    m_up->SetEnabled(inst != NULL);
    m_down->SetEnabled(inst != NULL);
    if (inst) {
        m_bypass->SetValue(pluginhost_is_active(inst) ? B_CONTROL_OFF : B_CONTROL_ON);
        m_mix->SetValue((int32)(pluginhost_get_mix(inst) * kSliderSteps));
    }
}

void FxWindow::UpdateValueLabel(guint param)
{
    PluginInstance *inst = Selected();
    if (!inst || param >= m_value_labels.size())
        return;
    char buf[128];
    pluginhost_param_display(inst, param, buf, sizeof(buf));
    m_value_labels[param]->SetText(buf);
}

static void fx_basename(const char *path, const char *prefix, char *out, size_t outlen)
{
    const char *base = path && *path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : (path && *path ? path : "(none)");
    snprintf(out, outlen, "%s%s", prefix, base);
}

void FxWindow::UpdateFileLabels()
{
    PluginInstance *inst = Selected();
    if (!inst || !m_model_label || !m_ir_label)
        return;
    char path[1024], label[1100];
    pluginhost_file_get(inst, PH_FILE_MODEL, path, sizeof(path));
    fx_basename(path, "Model: ", label, sizeof(label));
    m_model_label->SetText(label);
    pluginhost_file_get(inst, PH_FILE_IR, path, sizeof(path));
    fx_basename(path, "IR: ", label, sizeof(label));
    m_ir_label->SetText(label);
}

void FxWindow::RebuildParamPanel()
{
    m_sliders.clear();
    m_value_labels.clear();
    m_model_label = NULL;
    m_ir_label = NULL;
    BView *child;
    while ((child = m_param_group->ChildAt(0)) != NULL) {
        m_param_group->RemoveChild(child);
        delete child;
    }

    PluginInstance *inst = Selected();
    SyncControlsRow();

    BLayoutBuilder::Group<> builder(m_param_group->GroupLayout());
    if (!inst) {
        builder.Add(new BStringView("hint", "Select or add an effect."));
        builder.AddGlue();
        return;
    }

    // File loading rows first (NAMku's model/IR — the slider-only equivalent
    // of the plug-in GUI's two folder buttons).
    if (pluginhost_has_file_loader(inst)) {
        m_model_label = new BStringView("model", "Model: (none)");
        m_ir_label = new BStringView("ir", "IR: (none)");
        BButton *load_model =
            new BButton("load-model", "Load model…", new BMessage(MSG_FX_LOAD_MODEL));
        BButton *load_ir = new BButton("load-ir", "Load IR…", new BMessage(MSG_FX_LOAD_IR));
        // clang-format off
        builder
            .AddGroup(B_HORIZONTAL, 6.0f)
                .Add(load_model)
                .Add(m_model_label)
                .AddGlue()
            .End()
            .AddGroup(B_HORIZONTAL, 6.0f)
                .Add(load_ir)
                .Add(m_ir_label)
                .AddGlue()
            .End();
        // clang-format on
        UpdateFileLabels();
    }

    guint n = pluginhost_param_count(inst);
    if (n == 0 && !pluginhost_has_file_loader(inst))
        builder.Add(new BStringView("none", "This plugin exposes no editable parameters."));

    for (guint i = 0; i < n; i++) {
        char name[128];
        pluginhost_param_name(inst, i, name, sizeof(name));

        BMessage *msg = new BMessage(MSG_FX_PARAM);
        msg->AddInt32("param", (int32)i);
        BSlider *slider = new BSlider("param", name, msg, 0, kSliderSteps, B_HORIZONTAL);
        BMessage *mod = new BMessage(MSG_FX_PARAM);
        mod->AddInt32("param", (int32)i);
        slider->SetModificationMessage(mod);
        slider->SetValue((int32)(pluginhost_param_get(inst, i) * kSliderSteps));

        BStringView *value = new BStringView("value", "");
        value->SetExplicitMinSize(BSize(110.0f, B_SIZE_UNSET));

        // clang-format off
        builder
            .AddGroup(B_HORIZONTAL, 6.0f)
                .Add(slider)
                .Add(value)
            .End();
        // clang-format on

        m_sliders.push_back(slider);
        m_value_labels.push_back(value);
        UpdateValueLabel(i);
    }
    builder.AddGlue();
}

void FxWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_FX_RAISE:
            Activate();
            break;

        case MSG_FX_ADD: {
            const char *key = NULL, *name = NULL, *category = NULL;
            message->FindString("key", &key);
            message->FindString("name", &name);
            message->FindString("category", &category);
            if (!key || !name)
                break;
            PluginInfo info;
            info.format = PH_VST3;
            info.key = (char *)key;
            info.name = (char *)name;
            info.category = (char *)(category ? category : "");
            PluginInstance *inst = pluginhost_instantiate(&info);
            if (!inst) {
                g_warning("FxWindow: could not instantiate '%s'", name);
                break;
            }
            jackdaw_track_fx_add(m_track, inst);
            RebuildChainList((int)jackdaw_track_fx_count(m_track) - 1);
            break;
        }

        case MSG_FX_SELECT:
            RebuildParamPanel();
            break;

        case MSG_FX_REMOVE: {
            int32 sel = m_chain_list->CurrentSelection();
            if (sel < 0)
                break;
            jackdaw_track_fx_remove(m_track, (guint)sel);
            guint n = jackdaw_track_fx_count(m_track);
            RebuildChainList(n > 0 ? MIN((int)n - 1, (int)sel) : -1);
            break;
        }

        case MSG_FX_UP:
        case MSG_FX_DOWN: {
            int32 sel = m_chain_list->CurrentSelection();
            if (sel < 0)
                break;
            int to = sel + (message->what == MSG_FX_UP ? -1 : 1);
            if (to < 0 || to >= (int)jackdaw_track_fx_count(m_track))
                break;
            jackdaw_track_fx_move(m_track, (guint)sel, (guint)to);
            RebuildChainList(to);
            break;
        }

        case MSG_FX_BYPASS: {
            PluginInstance *inst = Selected();
            if (!inst)
                break;
            pluginhost_set_active(inst, m_bypass->Value() != B_CONTROL_ON);
            RebuildChainList(m_chain_list->CurrentSelection());
            break;
        }

        case MSG_FX_MIX: {
            PluginInstance *inst = Selected();
            if (inst)
                pluginhost_set_mix(inst, (float)m_mix->Value() / kSliderSteps);
            break;
        }

        case MSG_FX_PARAM: {
            PluginInstance *inst = Selected();
            int32 param = -1;
            if (!inst || message->FindInt32("param", &param) != B_OK)
                break;
            if (param < 0 || (size_t)param >= m_sliders.size())
                break;
            float v = (float)m_sliders[param]->Value() / kSliderSteps;
            pluginhost_param_set(inst, (guint)param, v);
            UpdateValueLabel((guint)param);
            break;
        }

        case MSG_FX_LOAD_MODEL:
        case MSG_FX_LOAD_IR: {
            if (!m_file_panel)
                m_file_panel = new BFilePanel(B_OPEN_PANEL);
            BMessage tmpl(MSG_FX_FILE_REFS);
            tmpl.AddInt32("which", message->what == MSG_FX_LOAD_MODEL ? PH_FILE_MODEL : PH_FILE_IR);
            m_file_panel->SetTarget(BMessenger(this));
            m_file_panel->SetMessage(&tmpl);
            m_file_panel->Window()->SetTitle(message->what == MSG_FX_LOAD_MODEL
                                                 ? "Load NAM model (.nam)"
                                                 : "Load impulse response (.wav)");
            m_file_panel->Show();
            break;
        }

        case MSG_FX_FILE_REFS: {
            PluginInstance *inst = Selected();
            entry_ref ref;
            int32 which = PH_FILE_MODEL;
            message->FindInt32("which", &which);
            if (!inst || message->FindRef("refs", &ref) != B_OK)
                break;
            BPath path(&ref);
            if (path.InitCheck() != B_OK)
                break;
            if (!pluginhost_file_set(inst, (int)which, path.Path()))
                g_warning("FxWindow: plugin refused file '%s'", path.Path());
            UpdateFileLabels();
            break;
        }

        default:
            BWindow::MessageReceived(message);
            break;
    }
}
