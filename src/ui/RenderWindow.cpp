#include "RenderWindow.h"

#include <Alert.h>
#include <Button.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <StatusBar.h>
#include <String.h>
#include <TextControl.h>

#include <stdio.h>

#include "engine/render.h"
#include "engine/settings.h"
#include "Messages.h"

enum {
    MSG_RW_FORMAT = 'rwfm',
    MSG_RW_BROWSE = 'rwbr',
    MSG_RW_RENDER = 'rwrn',
    MSG_RW_CLOSE = 'rwcl',
    MSG_RW_SAVE_REF = 'rwsv',
};

static const int kSampleRates[] = {44100, 48000, 88200, 96000};
static const int kNumSampleRates = 4;

// Build a BMenuField whose items each carry the same `msg` (NULL for inert
// choices). The selected value is read back via the marked item's index.
static BMenuField *make_choice(const char *name, const char *label, const char *const *items, int n,
                               int sel, uint32 msg)
{
    BPopUpMenu *menu = new BPopUpMenu(label);
    for (int i = 0; i < n; i++) {
        BMenuItem *it = new BMenuItem(items[i], msg ? new BMessage(msg) : NULL);
        menu->AddItem(it);
        if (i == sel)
            it->SetMarked(true);
    }
    return new BMenuField(name, label, menu);
}

static int marked_index(BMenuField *f)
{
    if (!f || !f->Menu())
        return 0;
    BMenuItem *m = f->Menu()->FindMarked();
    return m ? f->Menu()->IndexOf(m) : 0;
}

RenderWindow::RenderWindow(JackDawProject *project, BMessenger main)
    : BWindow(BRect(0, 0, 460, 360), "Render / Export", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
      m_project(project), m_main(main), m_save_panel(NULL), m_running(false)
{
    RenderOptions seed = {};
    jackdaw_render_options_load(&seed);

    const char *formats[] = {"WAV", "FLAC", "MP3"};
    const char *bits[] = {"16-bit", "24-bit", "32-bit"};
    const char *sources[] = {"Master mix", "Selected tracks"};
    const char *scopes[] = {"Whole project", "Loop region"};
    const char *rates[] = {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz"};
    const char *chans[] = {"Stereo", "Mono"};
    const char *methods[] = {"Offline (faster)", "Realtime"};

    int rate_sel = 1; // 48000
    for (int i = 0; i < kNumSampleRates; i++)
        if (kSampleRates[i] == seed.sample_rate)
            rate_sel = i;

    m_format = make_choice("format", "Format:", formats, 3, (int)seed.format, MSG_RW_FORMAT);
    m_bits = make_choice("bits", "Bit depth:", bits, 3, (int)seed.bit_depth, 0);
    m_source = make_choice("source", "Source:", sources, 2, (int)seed.source, 0);
    m_scope = make_choice("scope", "Scope:", scopes, 2, RENDER_SCOPE_PROJECT, 0);
    m_rate = make_choice("rate", "Sample rate:", rates, kNumSampleRates, rate_sel, 0);
    m_channels = make_choice("chans", "Channels:", chans, 2, seed.channels == 1 ? 1 : 0, 0);
    m_method = make_choice("method", "Method:", methods, 2, (int)seed.method, 0);

    m_out = new BTextControl("out", "Output file:", "", NULL);
    m_browse = new BButton("browse", "Browse…", new BMessage(MSG_RW_BROWSE));

    m_status = new BStatusBar("status", "Idle");
    m_status->SetMaxValue(100.0f);

    m_render = new BButton("render", "Render", new BMessage(MSG_RW_RENDER));
    m_close = new BButton("close", "Close", new BMessage(MSG_RW_CLOSE));

    // Seed a default output path: last render dir + project name + extension.
    {
        gchar *dir = settings_get_string("render_last_dir", NULL);
        BString base("render");
        const gchar *pf = jackdaw_project_get_file(m_project);
        if (pf && *pf) {
            gchar *b = g_path_get_basename(pf);
            base = b;
            if (base.EndsWith(".jdaw"))
                base.Truncate(base.Length() - 5);
            g_free(b);
        }
        BString path;
        if (dir && *dir)
            path << dir << "/" << base;
        else
            path << base;
        path << "." << jackdaw_render_extension((RenderFormat)seed.format);
        m_out->SetText(path.String());
        g_free(dir);
    }

    // clang-format off
    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(m_format)
        .Add(m_bits)
        .Add(m_source)
        .Add(m_scope)
        .Add(m_rate)
        .Add(m_channels)
        .Add(m_method)
        .AddGroup(B_HORIZONTAL)
            .Add(m_out)
            .Add(m_browse)
        .End()
        .Add(m_status)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(m_close)
            .Add(m_render)
        .End();
    // clang-format on

    m_render->MakeDefault(true);
    RebuildFormatState();
    CenterOnScreen();
}

RenderWindow::~RenderWindow()
{
    delete m_save_panel;
}

int RenderWindow::SelectedFormat() const
{
    return marked_index(m_format);
}
int RenderWindow::SelectedBitDepth() const
{
    return marked_index(m_bits);
}
int RenderWindow::SelectedSource() const
{
    return marked_index(m_source);
}
int RenderWindow::SelectedScope() const
{
    return marked_index(m_scope);
}
int RenderWindow::SelectedMethod() const
{
    return marked_index(m_method);
}
int RenderWindow::SelectedChannels() const
{
    return marked_index(m_channels) == 1 ? 1 : 2;
}

int RenderWindow::SelectedSampleRate() const
{
    int i = marked_index(m_rate);
    if (i < 0 || i >= kNumSampleRates)
        i = 1;
    return kSampleRates[i];
}

// Bit depth applies only to WAV; grey out FLAC/MP3 when the running libsndfile
// can't encode them (no MPEG/FLAC support compiled in).
void RenderWindow::RebuildFormatState()
{
    int fmt = SelectedFormat();
    if (m_bits)
        m_bits->SetEnabled(fmt == RENDER_FMT_WAV);

    if (m_format && m_format->Menu()) {
        BMenu *menu = m_format->Menu();
        RenderOptions probe = {};
        probe.sample_rate = SelectedSampleRate();
        probe.channels = SelectedChannels();

        BMenuItem *flac = menu->ItemAt(RENDER_FMT_FLAC);
        if (flac) {
            probe.format = RENDER_FMT_FLAC;
            flac->SetEnabled(jackdaw_render_format_supported(&probe));
        }
        BMenuItem *mp3 = menu->ItemAt(RENDER_FMT_MP3);
        if (mp3)
            mp3->SetEnabled(jackdaw_render_mp3_available(probe.sample_rate, probe.channels));
    }
}

void RenderWindow::SyncOutputExtension()
{
    BString path(m_out->Text());
    int dot = path.FindLast('.');
    int slash = path.FindLast('/');
    if (dot > slash) // strip a trailing extension only (not a dot in a dir name)
        path.Truncate(dot);
    path << "." << jackdaw_render_extension((RenderFormat)SelectedFormat());
    m_out->SetText(path.String());
}

void RenderWindow::SetRunning(bool running)
{
    m_running = running;
    if (m_render)
        m_render->SetEnabled(!running);
    if (m_format)
        m_format->SetEnabled(!running);
    if (m_source)
        m_source->SetEnabled(!running);
    if (m_scope)
        m_scope->SetEnabled(!running);
    if (m_rate)
        m_rate->SetEnabled(!running);
    if (m_channels)
        m_channels->SetEnabled(!running);
    if (m_method)
        m_method->SetEnabled(!running);
    if (m_out)
        m_out->SetEnabled(!running);
    if (m_browse)
        m_browse->SetEnabled(!running);
    if (m_close)
        m_close->SetLabel(running ? "Cancel" : "Close");
    if (!running)
        RebuildFormatState();
}

void RenderWindow::StartRender()
{
    const char *path = m_out->Text();
    if (!path || !*path) {
        (new BAlert("Render", "Please choose an output file.", "OK", NULL, NULL, B_WIDTH_AS_USUAL,
                    B_WARNING_ALERT))
            ->Go();
        return;
    }

    m_status->Reset("Rendering…");
    m_status->SetMaxValue(100.0f);

    BMessage out(MSG_RENDER_START);
    out.AddInt32("format", SelectedFormat());
    out.AddInt32("bit_depth", SelectedBitDepth());
    out.AddInt32("source", SelectedSource());
    out.AddInt32("scope", SelectedScope());
    out.AddInt32("method", SelectedMethod());
    out.AddInt32("sample_rate", SelectedSampleRate());
    out.AddInt32("channels", SelectedChannels());
    out.AddString("out_path", path);
    m_main.SendMessage(&out);
    SetRunning(true);
}

void RenderWindow::Present(bool region_scope)
{
    if (m_scope && m_scope->Menu()) {
        BMenuItem *it =
            m_scope->Menu()->ItemAt(region_scope ? RENDER_SCOPE_REGION : RENDER_SCOPE_PROJECT);
        if (it)
            it->SetMarked(true);
    }
    if (!m_running) {
        m_status->Reset("Idle");
        m_status->SetMaxValue(100.0f); // Reset() zeroes the max
        SetRunning(false);
    }
    if (IsHidden())
        Show();
    Activate(true);
}

void RenderWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_RW_FORMAT:
            SyncOutputExtension();
            RebuildFormatState();
            break;

        case MSG_RW_BROWSE: {
            if (!m_save_panel) {
                m_save_panel = new BFilePanel(B_SAVE_PANEL, new BMessenger(this), NULL, 0, false);
                BMessage m(MSG_RW_SAVE_REF);
                m_save_panel->SetMessage(&m);
            }
            BPath cur(m_out->Text());
            BPath parent;
            if (cur.GetParent(&parent) == B_OK) {
                BEntry pe(parent.Path(), true);
                entry_ref pr;
                if (pe.GetRef(&pr) == B_OK)
                    m_save_panel->SetPanelDirectory(&pr);
            }
            if (cur.Leaf())
                m_save_panel->SetSaveText(cur.Leaf());
            m_save_panel->Show();
            break;
        }
        case MSG_RW_SAVE_REF: {
            entry_ref dir;
            const char *name = NULL;
            if (message->FindRef("directory", &dir) == B_OK &&
                message->FindString("name", &name) == B_OK && name && *name) {
                BPath p(&dir);
                p.Append(name);
                m_out->SetText(p.Path());
                SyncOutputExtension();
            }
            break;
        }

        case MSG_RW_RENDER:
            if (!m_running)
                StartRender();
            break;

        case MSG_RW_CLOSE:
            if (m_running)
                m_main.SendMessage(MSG_RENDER_CANCEL);
            else if (!IsHidden())
                Hide();
            break;

        case MSG_RENDER_PROGRESS: {
            double frac = 0.0;
            int32 state = 0;
            message->FindDouble("frac", &frac);
            message->FindInt32("state", &state);
            float target = (float)(frac * 100.0);
            if (target < 0.0f)
                target = 0.0f;
            if (target > 100.0f)
                target = 100.0f;
            float curv = m_status->CurrentValue();
            m_status->Update(target - curv);
            if (state != 0) {
                const char *txt = state == 1   ? "Finished"
                                  : state == 2 ? "Cancelled"
                                               : "Render failed — check the path/format";
                m_status->Reset(txt);
                m_status->SetMaxValue(100.0f); // Reset() zeroes the max
                if (state == 1)
                    m_status->Update(100.0f);
                SetRunning(false);
            }
            break;
        }

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool RenderWindow::QuitRequested()
{
    // Hide instead of destroy so a running render keeps reporting; the main
    // window force-quits this window on app shutdown.
    if (m_running)
        m_main.SendMessage(MSG_RENDER_CANCEL);
    if (!IsHidden())
        Hide();
    return false;
}
