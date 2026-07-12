#include "MidiWindow.h"

#include <Button.h>
#include <Cursor.h>
#include <InterfaceDefs.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <SeparatorView.h>
#include <StringView.h>

#include <math.h>
#include <stdio.h>

#include "engine/jackdaw-engine.h"
#include "engine/timeruler.h"
#include "MainWindow.h"
#include "Messages.h"
#include "StateButton.h"

// Geometry (mirrors the Linux editor).
static const float kKeyW = 42.0f;   // keyboard column width
static const float kVelH = 110.0f;  // velocity lane height
static const float kRulerH = 20.0f; // time ruler height
static const int kDefaultKeyH = 8;  // px per semitone row
static const float kEdgePx = 6.0f;  // note-resize grab zone
static const int kDefaultVel = 100; // velocity for mouse-created notes
static const float kVelBarW = 8.0f; // velocity bar hit zone (drawn 2 px)
static const double kHMaxTicks = (double)JACKDAW_PPQ * 4 * 1000;

// Window-internal message codes.
enum {
    MSG_MW_TICK = 'mwtk',
    MSG_MW_PLAY = 'mwpl',
    MSG_MW_LOOP = 'mwlp',
    MSG_MW_PAUSE = 'mwpa',
    MSG_MW_STOP = 'mwst',
    MSG_MW_RTZ = 'mwrz',
    MSG_MW_STEP_BACK = 'mwsb',
    MSG_MW_STEP_FWD = 'mwsf',
    MSG_MW_NEXT = 'mwnx',
    MSG_MW_SNAP = 'mwsn',
};

// The clip's dominant MIDI channel. New and auditioned notes follow the
// existing content (e.g. channel 10 for a take recorded from a drum kit) so
// they reach the same voice on the connected module; 0 for an empty clip.
// Caller holds the main lock (clip is model state).
static guint8 ClipDefaultChannel(MidiClip *clip)
{
    if (!clip || !clip->notes || clip->notes->len == 0)
        return 0;
    guint counts[16] = {0};
    for (guint i = 0; i < clip->notes->len; i++) {
        const MidiNote *n = &g_array_index(clip->notes, MidiNote, i);
        counts[n->channel & 0x0F]++;
    }
    guint8 best = 0;
    for (guint8 c = 1; c < 16; c++)
        if (counts[c] > counts[best])
            best = c;
    return best;
}

// velocity 1..127 -> blue(low) .. green .. red(high), same ramp as Linux.
static rgb_color VelColor(int v)
{
    double f = CLAMP(v, 1, 127) / 127.0;
    double r, g, b;
    if (f < 0.5) {
        double u = f / 0.5;
        r = 0.1;
        g = 0.3 + 0.6 * u;
        b = 0.9 - 0.7 * u;
    } else {
        double u = (f - 0.5) / 0.5;
        r = 0.1 + 0.85 * u;
        g = 0.9 - 0.7 * u;
        b = 0.15;
    }
    rgb_color c = {(uint8)(r * 255.0), (uint8)(g * 255.0), (uint8)(b * 255.0), 255};
    return c;
}

static bool IsBlackKey(int p)
{
    int n = p % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// Human-readable note name, e.g. pitch 60 -> "C4" (octave = pitch/12 - 1).
static void NoteName(int pitch, char *buf, size_t n)
{
    static const char *names[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};
    pitch = CLAMP(pitch, 0, 127);
    snprintf(buf, n, "%s%d", names[pitch % 12], pitch / 12 - 1);
}

// ---------------------------------------------------------------------------
// Child views: thin BView shells that forward every hook to the window, which
// holds all editor state (the Linux editor's MidiWindow struct, one thread).

class MidiRollView : public BView
{
public:
    explicit MidiRollView(MidiWindow *mw)
        : BView("midi_roll", B_WILL_DRAW | B_FRAME_EVENTS), m_mw(mw)
    {
    }
    void Draw(BRect r) override
    {
        (void)r;
        m_mw->RollDraw(this, Bounds());
    }
    void MouseDown(BPoint where) override
    {
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        m_mw->RollMouseDown(this, where);
    }
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override
    {
        (void)transit;
        (void)drag;
        m_mw->RollMouseMoved(this, where);
    }
    void MouseUp(BPoint where) override
    {
        m_mw->RollMouseUp(this, where);
    }
    void MessageReceived(BMessage *message) override
    {
        if (message->what == B_MOUSE_WHEEL_CHANGED) {
            float dy = 0.0f;
            if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
                m_mw->RollWheel(dy, modifiers());
                return;
            }
        }
        BView::MessageReceived(message);
    }
    void FrameResized(float w, float h) override
    {
        BView::FrameResized(w, h);
        m_mw->UpdateScrollbars();
    }

private:
    MidiWindow *m_mw;
};

class MidiKeysView : public BView
{
public:
    explicit MidiKeysView(MidiWindow *mw) : BView("midi_keys", B_WILL_DRAW), m_mw(mw)
    {
    }
    void Draw(BRect r) override
    {
        (void)r;
        m_mw->KeysDraw(this, Bounds());
    }
    void MouseDown(BPoint where) override
    {
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        m_mw->KeysMouseDown(this, where);
    }
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override
    {
        (void)transit;
        (void)drag;
        m_mw->KeysMouseMoved(this, where);
    }
    void MouseUp(BPoint where) override
    {
        m_mw->KeysMouseUp(this, where);
    }

private:
    MidiWindow *m_mw;
};

class MidiVelView : public BView
{
public:
    explicit MidiVelView(MidiWindow *mw) : BView("midi_vel", B_WILL_DRAW), m_mw(mw)
    {
    }
    void Draw(BRect r) override
    {
        (void)r;
        m_mw->VelDraw(this, Bounds());
    }
    void MouseDown(BPoint where) override
    {
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        m_mw->VelMouseDown(this, where);
    }
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override
    {
        (void)transit;
        (void)drag;
        m_mw->VelMouseMoved(this, where);
    }
    void MouseUp(BPoint where) override
    {
        m_mw->VelMouseUp(this, where);
    }

private:
    MidiWindow *m_mw;
};

class MidiRollRulerView : public BView
{
public:
    explicit MidiRollRulerView(MidiWindow *mw) : BView("midi_ruler", B_WILL_DRAW), m_mw(mw)
    {
    }
    void Draw(BRect r) override
    {
        (void)r;
        m_mw->RulerDraw(this, Bounds());
    }
    void MouseDown(BPoint where) override
    {
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        m_mw->RulerMouseDown(this, where);
    }
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override
    {
        (void)transit;
        (void)drag;
        m_mw->RulerMouseMoved(this, where);
    }
    void MouseUp(BPoint where) override
    {
        m_mw->RulerMouseUp(this, where);
    }

private:
    MidiWindow *m_mw;
};

// Scrollbars drive the window's h/v origin directly (TimelineView pattern).
class MidiHScrollBar : public BScrollBar
{
public:
    explicit MidiHScrollBar(MidiWindow *mw)
        : BScrollBar("midi_hscroll", NULL, 0, 0, B_HORIZONTAL), m_mw(mw)
    {
    }
    void ValueChanged(float v) override
    {
        BScrollBar::ValueChanged(v);
        m_mw->SetHTick((double)v);
    }

private:
    MidiWindow *m_mw;
};

class MidiVScrollBar : public BScrollBar
{
public:
    explicit MidiVScrollBar(MidiWindow *mw)
        : BScrollBar("midi_vscroll", NULL, 0, 0, B_VERTICAL), m_mw(mw)
    {
    }
    void ValueChanged(float v) override
    {
        BScrollBar::ValueChanged(v);
        m_mw->SetVRow((double)v);
    }

private:
    MidiWindow *m_mw;
};

// Keyboard shortcuts. Haiku resolves menu/window shortcuts only while the
// Command key is held, so Ctrl combos are caught here via raw_char (the base
// letter; "byte" would be the control code under Ctrl).
class MidiKeyFilter : public BMessageFilter
{
public:
    MidiKeyFilter(MidiWindow *mw, BMessenger main)
        : BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_KEY_DOWN), m_mw(mw), m_main(main)
    {
    }
    filter_result Filter(BMessage *message, BHandler **target) override
    {
        (void)target;
        int32 mods = 0, raw = 0;
        message->FindInt32("modifiers", &mods);
        message->FindInt32("raw_char", &raw);
        if (mods & B_CONTROL_KEY) {
            switch (raw) {
                case 'a':
                    m_mw->SelectAll();
                    return B_SKIP_MESSAGE;
                case 'c':
                    m_mw->CopySelection();
                    return B_SKIP_MESSAGE;
                case 'v':
                    m_mw->PasteAtPlayhead();
                    return B_SKIP_MESSAGE;
                case 'z':
                    m_main.SendMessage((mods & B_SHIFT_KEY) ? MSG_EDIT_REDO : MSG_EDIT_UNDO);
                    return B_SKIP_MESSAGE;
                case 'y':
                    m_main.SendMessage(MSG_EDIT_REDO);
                    return B_SKIP_MESSAGE;
            }
            return B_DISPATCH_MESSAGE;
        }
        int8 byte = 0;
        if (message->FindInt8("byte", &byte) != B_OK)
            return B_DISPATCH_MESSAGE;
        switch (byte) {
            case B_SPACE:
                m_main.SendMessage(MSG_TRANSPORT_TOGGLE);
                return B_SKIP_MESSAGE;
            case B_HOME:
                m_mw->LocateStart();
                return B_SKIP_MESSAGE;
            case 'q':
            case 'Q':
                m_mw->Quantize();
                return B_SKIP_MESSAGE;
            case B_ESCAPE:
                m_mw->ClearSelection();
                return B_SKIP_MESSAGE;
        }
        return B_DISPATCH_MESSAGE;
    }

private:
    MidiWindow *m_mw;
    BMessenger m_main;
};

// ---------------------------------------------------------------------------

MidiWindow::MidiWindow(JackDawTrack *track, JackDawProject *project, BWindow *main)
    : BWindow(BRect(120, 100, 1020, 680), "Piano Roll", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS),
      m_track(track), m_project(project), m_clip(NULL), m_main(main), m_main_msgr(main),
      m_roll(NULL), m_keys(NULL), m_vel(NULL), m_ruler(NULL), m_hscroll(NULL), m_vscroll(NULL),
      m_btn_play(NULL), m_btn_loop(NULL), m_btn_snap(NULL), m_time_label(NULL), m_runner(NULL),
      m_tpx(20.0), m_key_h(kDefaultKeyH), m_h_tick(0.0), m_v_row(48.0), m_fpb(0.0), m_bpb(4),
      m_chan(0), m_drag_mode(0), m_drag_note(-1), m_press_x(0), m_press_y(0), m_orig_start(0),
      m_orig_len(0), m_orig_pitch(0), m_ctx_note_idx(-1), m_sel_dragging(false), m_sel_moved(false),
      m_sel_x0(0), m_sel_y0(0), m_sel_x1(0), m_sel_y1(0), m_hl_pitch(-1), m_key_playing(-1),
      m_play_tick(-1.0), m_prev_play_pos(-1), m_ruler_dragging(false), m_loop_drag_edge(0)
{
    // Constructed on the main window's looper, which owns the model.
    g_object_ref(m_track);
    m_clip = jackdaw_track_get_midi_clip(m_track);
    m_fpb = jackdaw_project_frames_per_beat(m_project, jackdaw_engine_get_sample_rate());
    m_bpb = m_project->beats_per_bar ? m_project->beats_per_bar : 4;
    m_chan = ClipDefaultChannel(m_clip);

    char title[160];
    snprintf(title, sizeof(title), "Piano Roll: %s", jackdaw_track_get_name(m_track));
    SetTitle(title);

    // --- Transport toolbar — same buttons/order as the main window, no recording.
    BButton *btn_start = new BButton("mw_rtz", "|◀", new BMessage(MSG_MW_RTZ));
    BButton *btn_back = new BButton("mw_back", "|<<", new BMessage(MSG_MW_STEP_BACK));
    BButton *btn_fwd = new BButton("mw_fwd", ">>|", new BMessage(MSG_MW_STEP_FWD));
    BButton *btn_next = new BButton("mw_next", "▶|", new BMessage(MSG_MW_NEXT));
    m_btn_play = new BButton("mw_play", "▶", new BMessage(MSG_MW_PLAY));
    m_btn_play->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    // Loop keeps "⟳" as a label for layout sizing only; the icon is vector-drawn
    // (GLYPH_LOOP) so it never depends on a font carrying that glyph.
    m_btn_loop = new StateButton("mw_loop", "⟳", new BMessage(MSG_MW_LOOP));
    m_btn_loop->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_btn_loop->SetGlyph(StateButton::GLYPH_LOOP);
    m_btn_loop->SetActiveColor((rgb_color){140, 230, 140, 255}, (rgb_color){16, 16, 16, 255});
    BButton *btn_pause = new BButton("mw_pause", "||", new BMessage(MSG_MW_PAUSE));
    BButton *btn_stop = new BButton("mw_stop", "■", new BMessage(MSG_MW_STOP));
    m_time_label = new BStringView("mw_time", "00:00.0");
    m_btn_snap = new BButton("mw_snap", "Snap", new BMessage(MSG_MW_SNAP));
    m_btn_snap->SetBehavior(BButton::B_TOGGLE_BEHAVIOR);
    m_btn_snap->SetValue(m_project->snap_enabled ? B_CONTROL_ON : B_CONTROL_OFF);

    m_ruler = new MidiRollRulerView(this);
    m_ruler->SetExplicitMinSize(BSize(B_SIZE_UNSET, kRulerH));
    m_ruler->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, kRulerH));
    m_keys = new MidiKeysView(this);
    m_keys->SetExplicitMinSize(BSize(kKeyW, B_SIZE_UNSET));
    m_keys->SetExplicitMaxSize(BSize(kKeyW, B_SIZE_UNLIMITED));
    m_roll = new MidiRollView(this);
    m_vel = new MidiVelView(this);
    m_vel->SetExplicitMinSize(BSize(B_SIZE_UNSET, kVelH));
    m_vel->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, kVelH));
    m_hscroll = new MidiHScrollBar(this);
    m_vscroll = new MidiVScrollBar(this);

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 4)
        .SetInsets(4, 3, 4, 3)
        .Add(btn_start)
        .Add(btn_back)
        .Add(btn_fwd)
        .Add(btn_next)
        .Add(new BSeparatorView(B_VERTICAL))
        .Add(m_btn_play)
        .Add(m_btn_loop)
        .Add(btn_pause)
        .Add(btn_stop)
        .Add(m_time_label)
        .AddGlue()
        .Add(m_btn_snap)
        .End()
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGroup(B_HORIZONTAL, 0)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(kKeyW))
        .Add(m_ruler)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(B_V_SCROLL_BAR_WIDTH))
        .End()
        .AddGroup(B_HORIZONTAL, 0)
        .Add(m_keys)
        .Add(m_roll)
        .Add(m_vscroll)
        .End()
        .AddGroup(B_HORIZONTAL, 0)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(kKeyW))
        .Add(m_vel)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(B_V_SCROLL_BAR_WIDTH))
        .End()
        .AddGroup(B_HORIZONTAL, 0)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(kKeyW))
        .Add(m_hscroll)
        .Add(BSpaceLayoutItem::CreateHorizontalStrut(B_V_SCROLL_BAR_WIDTH))
        .End();

    AddCommonFilter(new MidiKeyFilter(this, m_main_msgr));

    // 50 ms playhead/auto-scroll/transport-sync timer (Linux editor's rate).
    m_runner = new BMessageRunner(BMessenger(this), BMessage(MSG_MW_TICK), 50000);

    UpdateScrollbars();
}

MidiWindow::~MidiWindow()
{
    delete m_runner;
    // Deregister from the track + main window under the main lock (idempotent:
    // only if this window is still the registered editor). Runs on whichever
    // thread called Quit(); the main lock is never held by that caller here —
    // the main window's shutdown path drops its own lock while waiting for
    // this unregistration instead of locking editor windows.
    m_main->Lock();
    if (g_object_get_data(G_OBJECT(m_track), "midi-window") == this)
        g_object_set_data(G_OBJECT(m_track), "midi-window", NULL);
    static_cast<MainWindow *>(m_main)->UnregisterMidiEditor(this);
    m_main->Unlock();
    g_object_unref(m_track);
}

bool MidiWindow::QuitRequested()
{
    // Release any auditioned note before going away.
    if (m_key_playing >= 0) {
        BMessage off(MSG_MIDI_PREVIEW);
        off.AddPointer("track", m_track);
        off.AddInt8("pitch", (int8)m_key_playing);
        off.AddInt8("velocity", 0);
        off.AddInt8("channel", (int8)m_chan);
        off.AddBool("on", false);
        m_main_msgr.SendMessage(&off);
        m_key_playing = -1;
    }
    return true; // per-track editor really closes (not a hide-singleton)
}

// ---- helpers (Linux editor parity) ----------------------------------------

int MidiWindow::SnapStep() const
{
    // Caller holds the main lock (project field).
    if (m_project && !m_project->snap_enabled)
        return 1;
    return JACKDAW_PPQ / 4;
}

guint32 MidiWindow::SnapTick(double t) const
{
    int s = SnapStep();
    if (s <= 1 || t < 0)
        return (guint32)(t < 0 ? 0 : t);
    return (guint32)(floor(t / s + 0.5) * s);
}

double MidiWindow::XToTick(double x) const
{
    double t = m_h_tick + x * m_tpx;
    return t < 0 ? 0 : t;
}

int MidiWindow::YToPitch(double y) const
{
    int row = (int)floor(m_v_row + y / m_key_h);
    return CLAMP(127 - row, 0, 127);
}

double MidiWindow::VisibleTicks() const
{
    float w = m_roll->Bounds().Width();
    return (w > 1.0f ? (double)w : 1.0) * m_tpx;
}

void MidiWindow::RedrawAll()
{
    m_roll->Invalidate();
    m_keys->Invalidate();
    m_vel->Invalidate();
    m_ruler->Invalidate();
}

// Republish the RT snapshot + repaint. Caller holds the main lock.
void MidiWindow::Commit()
{
    double fpb = jackdaw_project_frames_per_beat(m_project, jackdaw_engine_get_sample_rate());
    jackdaw_track_commit_midi(m_track, fpb);
    m_roll->Invalidate();
    m_vel->Invalidate();
    m_ruler->Invalidate();
}

// ---- undo (memento = deep clip copy, routed through the global manager) ----

typedef struct {
    JackDawTrack *track;
    JackDawProject *project;
} MidiUndoCtx;

static double midi_undo_fpb(MidiUndoCtx *c)
{
    return jackdaw_project_frames_per_beat(c->project, jackdaw_engine_get_sample_rate());
}

static gpointer midi_undo_capture(gpointer ctx)
{
    MidiUndoCtx *c = (MidiUndoCtx *)ctx;
    return midi_clip_copy(jackdaw_track_get_midi_clip(c->track));
}

static void midi_undo_restore(gpointer ctx, gpointer state)
{
    MidiUndoCtx *c = (MidiUndoCtx *)ctx;
    // set_midi_clip consumes its argument; hand it a copy so the memento (and
    // its post-edit successor after the swap) stays intact for redo.
    jackdaw_track_set_midi_clip(c->track, midi_clip_copy((MidiClip *)state), midi_undo_fpb(c));
}

static void midi_undo_free(gpointer state)
{
    midi_clip_free((MidiClip *)state);
}

static void midi_undo_after(gpointer ctx)
{
    // Runs on the main looper (undo/redo execute there). The clip pointer was
    // replaced: tell the open editor (other thread) to refresh, asynchronously.
    MidiUndoCtx *c = (MidiUndoCtx *)ctx;
    MidiWindow *mw = (MidiWindow *)g_object_get_data(G_OBJECT(c->track), "midi-window");
    if (mw)
        mw->PostMessage(MSG_MIDI_EDITOR_REFRESH);
}

static void midi_undo_ctx_free(gpointer ctx)
{
    g_free(ctx);
}

// Caller holds the main lock (undo manager + clip).
void MidiWindow::PushUndo(const char *desc)
{
    if (!m_project)
        return;
    MidiUndoCtx *c = g_new0(MidiUndoCtx, 1);
    c->track = m_track;
    c->project = m_project;
    JackDawUndoAction a;
    a.ctx = c;
    a.saved_state = midi_clip_copy(jackdaw_track_get_midi_clip(m_track));
    a.capture_fn = midi_undo_capture;
    a.restore_fn = midi_undo_restore;
    a.free_fn = midi_undo_free;
    a.after_fn = midi_undo_after;
    a.ctx_free_fn = midi_undo_ctx_free;
    a.desc = g_strdup(desc ? desc : "MIDI edit");
    undo_manager_push(jackdaw_project_get_undo(m_project), &a);
}

void MidiWindow::RefreshAfterUndo()
{
    LockMain();
    m_clip = jackdaw_track_get_midi_clip(m_track);
    UnlockMain();
    std::fill(m_sel.begin(), m_sel.end(), 0);
    m_drag_mode = 0;
    m_drag_note = -1;
    RedrawAll();
}

// ---- selection --------------------------------------------------------------

// Topmost note whose rect contains (x,y); -1 if none. Caller holds main lock.
int MidiWindow::NoteAt(double x, double y, bool *on_edge) const
{
    if (on_edge)
        *on_edge = false;
    for (int i = (int)midi_clip_note_count(m_clip) - 1; i >= 0; i--) {
        MidiNote *n = midi_clip_note(m_clip, (guint)i);
        double nx = TickToX(n->start);
        double nw = (double)n->length / m_tpx;
        double ny = PitchToY(n->pitch);
        if (x >= nx && x <= nx + nw && y >= ny && y <= ny + m_key_h) {
            if (on_edge && x >= nx + nw - kEdgePx)
                *on_edge = true;
            return i;
        }
    }
    return -1;
}

void MidiWindow::SelEnsure()
{
    guint nc = midi_clip_note_count(m_clip);
    if (nc > m_sel.size())
        m_sel.resize(nc, 0);
}

bool MidiWindow::SelIs(guint i) const
{
    return i < m_sel.size() && m_sel[i];
}

guint MidiWindow::SelCount() const
{
    guint c = 0, nc = midi_clip_note_count(m_clip);
    for (guint i = 0; i < nc && i < m_sel.size(); i++)
        if (m_sel[i])
            c++;
    return c;
}

void MidiWindow::SelectAll()
{
    LockMain();
    SelEnsure();
    guint nc = midi_clip_note_count(m_clip);
    for (guint i = 0; i < nc; i++)
        m_sel[i] = 1;
    UnlockMain();
    m_roll->Invalidate();
    m_vel->Invalidate();
}

void MidiWindow::ClearSelection()
{
    std::fill(m_sel.begin(), m_sel.end(), 0);
    m_roll->Invalidate();
    m_vel->Invalidate();
}

// Snapshot every note's start/pitch/velocity so a group drag applies a uniform
// delta from the originals. Caller holds the main lock.
void MidiWindow::GrpCapture()
{
    guint nc = midi_clip_note_count(m_clip);
    m_grp_start.resize(nc);
    m_grp_pitch.resize(nc);
    m_grp_vel.resize(nc);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(m_clip, i);
        m_grp_start[i] = n->start;
        m_grp_pitch[i] = n->pitch;
        m_grp_vel[i] = n->velocity;
    }
}

// Recompute selection from the rubber-band rectangle. Caller holds main lock.
void MidiWindow::SelUpdateBox()
{
    SelEnsure();
    std::fill(m_sel.begin(), m_sel.end(), 0);
    double x0 = MIN(m_sel_x0, m_sel_x1), x1 = MAX(m_sel_x0, m_sel_x1);
    double y0 = MIN(m_sel_y0, m_sel_y1), y1 = MAX(m_sel_y0, m_sel_y1);
    guint nc = midi_clip_note_count(m_clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(m_clip, i);
        double nx = TickToX(n->start);
        double nw = (double)n->length / m_tpx;
        double ny = PitchToY(n->pitch);
        if (nx + nw >= x0 && nx <= x1 && ny + m_key_h >= y0 && ny <= y1)
            m_sel[i] = 1;
    }
}

// ---- edit operations --------------------------------------------------------

// Snap note starts to a 1/16 grid (always, independent of the Snap toggle).
// Selection-only when any notes are selected.
void MidiWindow::Quantize()
{
    LockMain();
    int step = JACKDAW_PPQ / 4;
    guint nc = midi_clip_note_count(m_clip);
    guint sc = SelCount();
    if (nc == 0) {
        UnlockMain();
        return;
    }
    PushUndo("Quantize");
    for (guint i = 0; i < nc; i++) {
        if (sc > 0 && !SelIs(i))
            continue;
        MidiNote *n = midi_clip_note(m_clip, i);
        n->start = (guint32)(floor((double)n->start / step + 0.5) * step);
    }
    Commit();
    UnlockMain();
}

void MidiWindow::DeleteSelectedOrCtx()
{
    LockMain();
    if (SelCount() > 0) {
        PushUndo("Delete notes");
        guint nc = midi_clip_note_count(m_clip);
        for (int i = (int)nc - 1; i >= 0; i--) // high -> low keeps indices valid
            if (SelIs((guint)i))
                midi_clip_remove_note(m_clip, (guint)i);
        std::fill(m_sel.begin(), m_sel.end(), 0);
        m_ctx_note_idx = -1;
        Commit();
    } else if (m_ctx_note_idx >= 0) {
        PushUndo("Delete note");
        midi_clip_remove_note(m_clip, (guint)m_ctx_note_idx);
        m_ctx_note_idx = -1;
        Commit();
    }
    UnlockMain();
}

// Copy the selected notes, normalizing starts so the earliest sits at tick 0.
void MidiWindow::CopySelection()
{
    LockMain();
    guint nc = midi_clip_note_count(m_clip);
    guint sc = SelCount();
    if (sc == 0) {
        UnlockMain();
        return;
    }
    guint32 minstart = G_MAXUINT32;
    for (guint i = 0; i < nc; i++)
        if (SelIs(i)) {
            guint32 s = midi_clip_note(m_clip, i)->start;
            if (s < minstart)
                minstart = s;
        }
    m_clipboard.clear();
    for (guint i = 0; i < nc; i++)
        if (SelIs(i)) {
            MidiNote n = *midi_clip_note(m_clip, i);
            n.start -= minstart;
            m_clipboard.push_back(n);
        }
    UnlockMain();
}

// Paste so the first note begins at the playhead (or the view's left edge when
// the playhead is off); the pasted notes become the new selection.
void MidiWindow::PasteAtPlayhead()
{
    if (m_clipboard.empty())
        return;
    LockMain();
    double base = (m_play_tick >= 0.0) ? m_play_tick : m_h_tick;
    guint32 origin = SnapTick(base);
    PushUndo("Paste notes");
    std::fill(m_sel.begin(), m_sel.end(), 0);
    for (guint i = 0; i < m_clipboard.size(); i++) {
        MidiNote n = m_clipboard[i];
        n.start = origin + n.start;
        guint idx = midi_clip_add_note(m_clip, n);
        SelEnsure();
        if (idx < m_sel.size())
            m_sel[idx] = 1;
    }
    Commit();
    UnlockMain();
}

void MidiWindow::ClearLoopRegion()
{
    BMessage msg(MSG_MIDI_SET_LOOP);
    msg.AddInt64("start", 0);
    msg.AddInt64("end", 0);
    msg.AddBool("disable", true);
    m_main_msgr.SendMessage(&msg);
    m_ruler->Invalidate();
    m_roll->Invalidate();
}

void MidiWindow::LocateStart()
{
    m_main_msgr.SendMessage(MSG_TRANSPORT_RTZ);
    m_play_tick = 0.0;
    SetHTick(0.0);
}

// ---- roll drawing ----------------------------------------------------------

void MidiWindow::RollDraw(BView *v, BRect bounds)
{
    LockMain(); // clip notes + cached tempo stay consistent while we paint

    v->SetHighColor(41, 41, 46);
    v->FillRect(bounds);

    // Horizontal semitone rows (black-key rows darker) + octave line at C.
    int top_row = (int)floor(m_v_row);
    for (int row = top_row;; row++) {
        int pitch = 127 - row;
        if (pitch < 0)
            break;
        float y = (float)((row - m_v_row) * m_key_h);
        if (y > bounds.bottom)
            break;
        if (IsBlackKey(pitch))
            v->SetHighColor(33, 33, 38);
        else
            v->SetHighColor(46, 46, 51);
        v->FillRect(BRect(0, y, bounds.right, y + m_key_h - 1));
        if (pitch % 12 == 0) {
            v->SetHighColor(18, 18, 20);
            v->StrokeLine(BPoint(0, y + m_key_h), BPoint(bounds.right, y + m_key_h));
        }
    }

    // Highlighted lane (selected piano key): faint amber band behind notes.
    if (m_hl_pitch >= 0) {
        float y = (float)PitchToY(m_hl_pitch);
        if (y + m_key_h >= 0 && y <= bounds.bottom) {
            v->SetDrawingMode(B_OP_ALPHA);
            v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            v->SetHighColor(242, 204, 51, 36);
            v->FillRect(BRect(0, y, bounds.right, y + m_key_h - 1));
            v->SetDrawingMode(B_OP_COPY);
        }
    }

    // Vertical beat/bar grid.
    long beat0 = (long)(m_h_tick / JACKDAW_PPQ);
    v->SetDrawingMode(B_OP_ALPHA);
    v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
    for (long beat = beat0;; beat++) {
        float x = (float)TickToX((double)beat * JACKDAW_PPQ);
        if (x > bounds.right)
            break;
        if (x < 0)
            continue;
        if (beat % (long)m_bpb == 0)
            v->SetHighColor(153, 153, 178, 140);
        else
            v->SetHighColor(128, 128, 128, 64);
        v->StrokeLine(BPoint(x, 0), BPoint(x, bounds.bottom));
    }
    v->SetDrawingMode(B_OP_COPY);

    // Notes, colored by velocity; selected notes get a white outline.
    guint nc = midi_clip_note_count(m_clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(m_clip, i);
        float nx = (float)TickToX(n->start);
        float nw = (float)((double)n->length / m_tpx);
        float ny = (float)PitchToY(n->pitch);
        if (nx + nw < 0 || nx > bounds.right)
            continue;
        if (nw < 2)
            nw = 2;
        BRect r(nx, ny + 0.5f, nx + nw, ny + m_key_h - 0.5f);
        v->SetHighColor(VelColor(n->velocity));
        v->FillRect(r);
        v->SetDrawingMode(B_OP_ALPHA);
        v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        v->SetHighColor(0, 0, 0, 153);
        v->StrokeRect(r);
        v->SetDrawingMode(B_OP_COPY);
        if (SelIs(i)) {
            v->SetHighColor(255, 255, 255);
            v->SetPenSize(2.0f);
            v->StrokeRect(BRect(nx + 1, ny + 1.5f, nx + nw - 1, ny + m_key_h - 1.5f));
            v->SetPenSize(1.0f);
        }
    }

    // Rubber-band selection box.
    if (m_sel_dragging && m_sel_moved) {
        float x0 = (float)MIN(m_sel_x0, m_sel_x1), x1 = (float)MAX(m_sel_x0, m_sel_x1);
        float y0 = (float)MIN(m_sel_y0, m_sel_y1), y1 = (float)MAX(m_sel_y0, m_sel_y1);
        v->SetDrawingMode(B_OP_ALPHA);
        v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        v->SetHighColor(115, 178, 255, 46);
        v->FillRect(BRect(x0, y0, x1, y1));
        v->SetHighColor(140, 204, 255, 230);
        v->StrokeRect(BRect(x0, y0, x1, y1));
        v->SetDrawingMode(B_OP_COPY);
    }

    // Loop-region band (faint amber over the grid).
    if (jackdaw_engine_has_loop_region() && m_fpb > 0.0) {
        off_t ls, le;
        jackdaw_engine_get_loop_range(&ls, &le);
        float x0 = (float)TickToX((double)ls * JACKDAW_PPQ / m_fpb);
        float x1 = (float)TickToX((double)le * JACKDAW_PPQ / m_fpb);
        x0 = CLAMP(x0, 0.0f, (float)bounds.right);
        x1 = CLAMP(x1, 0.0f, (float)bounds.right);
        if (x1 > x0) {
            bool on = jackdaw_engine_get_loop_enabled();
            v->SetDrawingMode(B_OP_ALPHA);
            v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            v->SetHighColor(242, 166, 26, on ? 26 : 13);
            v->FillRect(BRect(x0, 0, x1, bounds.bottom));
            v->SetDrawingMode(B_OP_COPY);
        }
    }

    // Playhead — on top.
    if (m_play_tick >= 0.0) {
        float cx = (float)TickToX(m_play_tick);
        if (cx >= 0 && cx <= bounds.right) {
            v->SetHighColor(255, 89, 0);
            v->StrokeLine(BPoint(cx, 0), BPoint(cx, bounds.bottom));
        }
    }

    UnlockMain();
}

// ---- keys drawing / interaction ---------------------------------------------

void MidiWindow::KeysDraw(BView *v, BRect bounds)
{
    v->SetHighColor(26, 26, 28);
    v->FillRect(bounds);
    BFont font(be_plain_font);
    font.SetSize(9.0f);
    v->SetFont(&font);
    int top_row = (int)floor(m_v_row);
    for (int row = top_row;; row++) {
        int pitch = 127 - row;
        if (pitch < 0)
            break;
        float y = (float)((row - m_v_row) * m_key_h);
        if (y > bounds.bottom)
            break;
        BRect r(0, y, bounds.right, y + m_key_h - 1);
        if (IsBlackKey(pitch))
            v->SetHighColor(20, 20, 23);
        else
            v->SetHighColor(217, 217, 217);
        v->FillRect(r);
        if (pitch == m_hl_pitch) { // selected key: amber overlay
            v->SetDrawingMode(B_OP_ALPHA);
            v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            v->SetHighColor(242, 184, 26, 153);
            v->FillRect(r);
            v->SetDrawingMode(B_OP_COPY);
        }
        v->SetDrawingMode(B_OP_ALPHA);
        v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        v->SetHighColor(0, 0, 0, 77);
        v->StrokeRect(r);
        v->SetDrawingMode(B_OP_COPY);
        if (pitch % 12 == 0 && m_key_h >= 8) {
            char buf[16];
            snprintf(buf, sizeof(buf), "C%d", pitch / 12 - 1);
            v->SetHighColor(51, 51, 51);
            v->DrawString(buf, BPoint(4, y + m_key_h - 2));
        }
    }
}

// Light up `pitch`'s lane and audition it on the track's MIDI output.
void MidiWindow::KeysPlay(int pitch)
{
    if (m_key_playing == pitch)
        return;
    if (m_key_playing >= 0) {
        BMessage off(MSG_MIDI_PREVIEW);
        off.AddPointer("track", m_track);
        off.AddInt8("pitch", (int8)m_key_playing);
        off.AddInt8("velocity", 0);
        off.AddInt8("channel", (int8)m_chan);
        off.AddBool("on", false);
        m_main_msgr.SendMessage(&off);
    }
    m_key_playing = pitch;
    m_hl_pitch = pitch;
    BMessage on(MSG_MIDI_PREVIEW);
    on.AddPointer("track", m_track);
    on.AddInt8("pitch", (int8)pitch);
    on.AddInt8("velocity", (int8)kDefaultVel);
    on.AddInt8("channel", (int8)m_chan);
    on.AddBool("on", true);
    m_main_msgr.SendMessage(&on);
    m_keys->Invalidate();
    m_roll->Invalidate();
}

void MidiWindow::KeysStop()
{
    if (m_key_playing >= 0) {
        BMessage off(MSG_MIDI_PREVIEW);
        off.AddPointer("track", m_track);
        off.AddInt8("pitch", (int8)m_key_playing);
        off.AddInt8("velocity", 0);
        off.AddInt8("channel", (int8)m_chan);
        off.AddBool("on", false);
        m_main_msgr.SendMessage(&off);
        m_key_playing = -1;
    }
}

void MidiWindow::KeysMouseDown(BView *v, BPoint where)
{
    (void)v;
    KeysPlay(YToPitch(where.y));
}

void MidiWindow::KeysMouseMoved(BView *v, BPoint where)
{
    // Note-name tooltip on hover; drag across keys = glissando.
    char buf[16];
    NoteName(YToPitch(where.y), buf, sizeof(buf));
    v->SetToolTip(buf);
    int32 buttons = 0;
    BMessage *msg = v->Window() ? v->Window()->CurrentMessage() : NULL;
    if (msg)
        msg->FindInt32("buttons", &buttons);
    if (buttons & B_PRIMARY_MOUSE_BUTTON)
        KeysPlay(YToPitch(where.y));
}

void MidiWindow::KeysMouseUp(BView *v, BPoint where)
{
    (void)v;
    (void)where;
    KeysStop(); // highlight persists; sound stops
}

// ---- velocity lane ------------------------------------------------------------

void MidiWindow::VelDraw(BView *v, BRect bounds)
{
    LockMain();
    v->SetHighColor(31, 31, 36);
    v->FillRect(bounds);
    v->SetDrawingMode(B_OP_ALPHA);
    v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
    v->SetHighColor(102, 102, 128, 153);
    v->StrokeLine(BPoint(0, 0), BPoint(bounds.right, 0));
    v->SetDrawingMode(B_OP_COPY);

    BFont font(be_plain_font);
    font.SetSize(9.0f);
    v->SetFont(&font);

    float h_total = bounds.Height();
    guint nc = midi_clip_note_count(m_clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(m_clip, i);
        float nx = (float)TickToX(n->start);
        if (nx < -kVelBarW || nx > bounds.right)
            continue;
        float h = (float)(n->velocity / 127.0) * (h_total - 4);
        bool selected = SelIs(i);
        if (selected) { // white halo behind the selected bar
            v->SetHighColor(255, 255, 255);
            v->SetPenSize(4.0f);
            v->StrokeLine(BPoint(nx + 1, h_total), BPoint(nx + 1, h_total - h));
        }
        v->SetHighColor(VelColor(n->velocity));
        v->SetPenSize(2.0f);
        v->StrokeLine(BPoint(nx + 1, h_total), BPoint(nx + 1, h_total - h));
        v->SetPenSize(1.0f);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", n->velocity);
        float label_y = h_total - h - 3;
        if (label_y < 11)
            label_y = 11;
        if (selected)
            v->SetHighColor(255, 255, 255);
        else
            v->SetHighColor(191, 191, 191);
        v->DrawString(buf, BPoint(nx - 2, label_y));
    }
    UnlockMain();
}

// Find the note whose bar is nearest to x, within the hit zone. Main-locked.
int MidiWindow::VelNoteAtX(double x) const
{
    int best = -1;
    double bestd = 1e18;
    guint nc = midi_clip_note_count(m_clip);
    for (guint i = 0; i < nc; i++) {
        MidiNote *n = midi_clip_note(m_clip, i);
        double d = fabs(TickToX(n->start) - x);
        if (d < bestd && d <= (double)kVelBarW) {
            bestd = d;
            best = (int)i;
        }
    }
    return best;
}

// Caller holds the main lock.
void MidiWindow::VelApplyY(int note_idx, double y)
{
    float h_total = m_vel->Bounds().Height();
    int val = (int)((1.0 - CLAMP(y, 0.0, (double)h_total) / (double)h_total) * 127.0 + 0.5);
    val = CLAMP(val, 1, 127);
    if (SelCount() > 0 && SelIs((guint)note_idx) && note_idx < (int)m_grp_vel.size()) {
        // Grabbed bar is selected: shift every selected note by the same delta
        // from its captured original, preserving relative dynamics.
        int delta = val - (int)m_grp_vel[note_idx];
        guint nc = midi_clip_note_count(m_clip);
        for (guint i = 0; i < nc && i < m_grp_vel.size(); i++)
            if (SelIs(i))
                midi_clip_note(m_clip, i)->velocity =
                    (guint8)CLAMP((int)m_grp_vel[i] + delta, 1, 127);
    } else {
        MidiNote *n = midi_clip_note(m_clip, (guint)note_idx);
        if (!n)
            return;
        n->velocity = (guint8)val;
    }
    Commit();
}

void MidiWindow::VelMouseDown(BView *v, BPoint where)
{
    (void)v;
    LockMain();
    int idx = VelNoteAtX(where.x);
    if (idx >= 0) {
        PushUndo("Velocity");
        m_drag_mode = 3;
        m_drag_note = idx;
        GrpCapture(); // originals for the relative group edit
        VelApplyY(idx, where.y);
    }
    UnlockMain();
}

void MidiWindow::VelMouseMoved(BView *v, BPoint where)
{
    int32 buttons = 0;
    BMessage *msg = v->Window() ? v->Window()->CurrentMessage() : NULL;
    if (msg)
        msg->FindInt32("buttons", &buttons);
    if ((buttons & B_PRIMARY_MOUSE_BUTTON) && m_drag_mode == 3 && m_drag_note >= 0) {
        LockMain();
        VelApplyY(m_drag_note, where.y);
        UnlockMain();
    }
}

void MidiWindow::VelMouseUp(BView *v, BPoint where)
{
    (void)v;
    (void)where;
    if (m_drag_mode == 3) {
        m_drag_mode = 0;
        m_drag_note = -1;
    }
}

// ---- ruler ------------------------------------------------------------------

void MidiWindow::RulerDraw(BView *v, BRect bounds)
{
    v->SetHighColor(28, 28, 33);
    v->FillRect(bounds);
    v->SetHighColor(89, 89, 115);
    v->StrokeLine(BPoint(0, bounds.bottom), BPoint(bounds.right, bounds.bottom));

    BFont font(be_plain_font);
    font.SetSize(10.0f);
    v->SetFont(&font);

    long beat0 = (long)(m_h_tick / JACKDAW_PPQ);
    for (long beat = beat0;; beat++) {
        float x = (float)TickToX((double)beat * JACKDAW_PPQ);
        if (x > bounds.right)
            break;
        if (x < 0)
            continue;
        if (beat % (long)m_bpb == 0) {
            long bar_num = beat / (long)m_bpb + 1;
            v->SetHighColor(166, 166, 199);
            v->StrokeLine(BPoint(x, 0), BPoint(x, bounds.bottom - 1));
            char buf[24];
            snprintf(buf, sizeof(buf), "%ld", bar_num);
            v->SetHighColor(217, 222, 242);
            v->DrawString(buf, BPoint(x + 3, bounds.bottom - 4));
        } else {
            v->SetHighColor(97, 97, 122);
            v->StrokeLine(BPoint(x, bounds.bottom * 0.55f), BPoint(x, bounds.bottom - 1));
        }
    }

    // Playhead — same orange as the note grid.
    if (m_play_tick >= 0.0) {
        float cx = (float)TickToX(m_play_tick);
        if (cx >= 0 && cx <= bounds.right) {
            v->SetHighColor(255, 89, 0);
            v->StrokeLine(BPoint(cx, 0), BPoint(cx, bounds.bottom));
        }
    }

    // Loop-region band + end tabs (amber), converted frames -> ticks.
    if (m_fpb > 0.0) {
        off_t ls, le;
        jackdaw_engine_get_loop_range(&ls, &le);
        bool has = jackdaw_engine_has_loop_region();
        bool on = jackdaw_engine_get_loop_enabled();
        float x0 = (float)TickToX((double)ls * JACKDAW_PPQ / m_fpb);
        float x1 = (float)TickToX((double)le * JACKDAW_PPQ / m_fpb);
        if (has && x1 >= 0 && x0 <= bounds.right) {
            float bx0 = CLAMP(x0, 0.0f, (float)bounds.right);
            float bx1 = CLAMP(x1, 0.0f, (float)bounds.right);
            v->SetDrawingMode(B_OP_ALPHA);
            v->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
            v->SetHighColor(242, 166, 26, on ? 77 : 38);
            v->FillRect(BRect(bx0, 0, bx1, bounds.bottom));
            v->SetDrawingMode(B_OP_COPY);
        }
        v->SetHighColor(242, 166, 26);
        if (x0 >= -5 && x0 <= bounds.right + 5)
            v->FillRect(BRect(x0, 0, x0 + 4, bounds.bottom));
        if (x1 >= -5 && x1 <= bounds.right + 5)
            v->FillRect(BRect(x1 - 4, 0, x1, bounds.bottom));
    }
}

// Move the engine playhead to the tick under ruler x (routed to the main
// window); reflect immediately rather than waiting for the next tick.
void MidiWindow::RulerSeekToX(double x)
{
    double tick = XToTick(x);
    if (tick < 0)
        tick = 0;
    if (m_fpb <= 0.0)
        return;
    off_t frame = (off_t)(tick * m_fpb / (double)JACKDAW_PPQ);
    BMessage msg(MSG_MIDI_LOCATE);
    msg.AddInt64("frame", (int64)frame);
    m_main_msgr.SendMessage(&msg);
    m_play_tick = tick;
    m_ruler->Invalidate();
    m_roll->Invalidate();
}

// Hit-test x against the loop tabs. 1 = start, 2 = end, 0 = neither. When no
// region exists both tabs sit at 0; a hit grabs the end tab so dragging right
// creates the region.
int MidiWindow::RulerLoopHit(double x) const
{
    if (m_fpb <= 0.0)
        return 0;
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    double x0 = TickToX((double)ls * JACKDAW_PPQ / m_fpb);
    double x1 = TickToX((double)le * JACKDAW_PPQ / m_fpb);
    if (!jackdaw_engine_has_loop_region())
        return (fabs(x - x0) <= 6.0) ? 2 : 0;
    if (fabs(x - x0) <= 6.0)
        return 1;
    if (fabs(x - x1) <= 6.0)
        return 2;
    return 0;
}

// Drag a loop tab to ruler x, snapped, clamped so edges cannot cross.
void MidiWindow::RulerLoopDragTo(double x)
{
    if (m_fpb <= 0.0)
        return;
    LockMain();
    double tick = SnapTick(XToTick(x));
    UnlockMain();
    if (tick < 0)
        tick = 0;
    off_t frame = (off_t)(tick * m_fpb / (double)JACKDAW_PPQ);
    off_t ls, le;
    jackdaw_engine_get_loop_range(&ls, &le);
    BMessage msg(MSG_MIDI_SET_LOOP);
    if (m_loop_drag_edge == 1) { // start tab
        if (frame > le)
            frame = le;
        msg.AddInt64("start", (int64)frame);
        msg.AddInt64("end", (int64)le);
    } else { // end tab
        if (frame < ls)
            frame = ls;
        msg.AddInt64("start", (int64)ls);
        msg.AddInt64("end", (int64)frame);
    }
    m_main_msgr.SendMessage(&msg);
    m_ruler->Invalidate();
    m_roll->Invalidate();
}

void MidiWindow::RulerMouseDown(BView *v, BPoint where)
{
    (void)v;
    int edge = RulerLoopHit(where.x);
    if (edge) {
        m_loop_drag_edge = edge;
        RulerLoopDragTo(where.x);
        return;
    }
    m_ruler_dragging = true;
    RulerSeekToX(where.x);
}

void MidiWindow::RulerMouseMoved(BView *v, BPoint where)
{
    int32 buttons = 0;
    BMessage *msg = v->Window() ? v->Window()->CurrentMessage() : NULL;
    if (msg)
        msg->FindInt32("buttons", &buttons);
    if (!(buttons & B_PRIMARY_MOUSE_BUTTON))
        return;
    if (m_loop_drag_edge)
        RulerLoopDragTo(where.x);
    else if (m_ruler_dragging)
        RulerSeekToX(where.x);
}

void MidiWindow::RulerMouseUp(BView *v, BPoint where)
{
    (void)v;
    (void)where;
    m_loop_drag_edge = 0;
    m_ruler_dragging = false;
}

// ---- roll interaction ---------------------------------------------------------

void MidiWindow::RollMouseDown(BView *v, BPoint where)
{
    int32 buttons = 0;
    BMessage *msg = v->Window() ? v->Window()->CurrentMessage() : NULL;
    if (msg)
        msg->FindInt32("buttons", &buttons);

    LockMain();
    bool edge = false;
    int idx = NoteAt(where.x, where.y, &edge);

    if (buttons & B_SECONDARY_MOUSE_BUTTON) {
        // Begin a potential rubber-band selection; if the pointer never moves,
        // MouseUp pops the context menu instead.
        m_sel_dragging = true;
        m_sel_moved = false;
        m_sel_x0 = m_sel_x1 = where.x;
        m_sel_y0 = m_sel_y1 = where.y;
        m_ctx_note_idx = idx;
        UnlockMain();
        return;
    }
    if (!(buttons & B_PRIMARY_MOUSE_BUTTON)) {
        UnlockMain();
        return;
    }

    // With an active selection, a left action operates on the selection
    // rather than creating notes.
    if (SelCount() > 0) {
        if (idx >= 0 && SelIs((guint)idx)) { // grab the group -> move it
            PushUndo("Move notes");
            GrpCapture();
            m_drag_mode = 4;
            m_drag_note = idx;
            m_press_x = where.x;
            m_press_y = where.y;
            UnlockMain();
            return;
        }
        std::fill(m_sel.begin(), m_sel.end(), 0); // clicked away: deselect only
        UnlockMain();
        m_roll->Invalidate();
        m_vel->Invalidate();
        return;
    }

    PushUndo("Edit note");
    if (idx < 0) { // empty: add a note
        MidiNote n;
        n.start = SnapTick(XToTick(where.x));
        n.length = SnapStep() > 1 ? (guint32)SnapStep() : JACKDAW_PPQ / 4;
        n.pitch = (guint8)YToPitch(where.y);
        n.velocity = kDefaultVel;
        n.channel = ClipDefaultChannel(m_clip); // follow the clip's voice
        m_chan = n.channel;
        idx = (int)midi_clip_add_note(m_clip, n);
        m_drag_mode = 1;
    } else {
        m_drag_mode = edge ? 2 : 1;
    }
    MidiNote *n = midi_clip_note(m_clip, (guint)idx);
    m_drag_note = idx;
    m_press_x = where.x;
    m_press_y = where.y;
    m_orig_start = n->start;
    m_orig_len = n->length;
    m_orig_pitch = n->pitch;
    Commit();
    UnlockMain();
}

void MidiWindow::RollMouseMoved(BView *v, BPoint where)
{
    if (m_sel_dragging) { // right-drag rubber band
        m_sel_x1 = where.x;
        m_sel_y1 = where.y;
        if (!m_sel_moved && (fabs(where.x - m_sel_x0) > 3.0 || fabs(where.y - m_sel_y0) > 3.0))
            m_sel_moved = true;
        if (m_sel_moved) {
            LockMain();
            SelUpdateBox();
            UnlockMain();
        }
        m_roll->Invalidate();
        m_vel->Invalidate();
        return;
    }
    if (m_drag_mode == 0 || m_drag_note < 0) {
        // Hover: east-west resize cursor over a note's right edge.
        LockMain();
        bool edge = false;
        NoteAt(where.x, where.y, &edge);
        UnlockMain();
        static BCursor resize(B_CURSOR_ID_RESIZE_EAST_WEST);
        static BCursor arrow(B_CURSOR_ID_SYSTEM_DEFAULT);
        v->SetViewCursor(edge ? &resize : &arrow);
        return;
    }

    LockMain();
    if (m_drag_mode == 4) { // move the whole selection
        double rawdt = (where.x - m_press_x) * m_tpx;
        int step = SnapStep();
        long dticks = (step > 1) ? (long)(floor(rawdt / step + 0.5) * step) : (long)rawdt;
        int dp = (int)floor((where.y - m_press_y) / m_key_h + 0.5);
        guint nc = midi_clip_note_count(m_clip);
        for (guint i = 0; i < nc && i < m_grp_start.size(); i++) {
            if (!SelIs(i))
                continue;
            MidiNote *gn = midi_clip_note(m_clip, i);
            long ns = (long)m_grp_start[i] + dticks;
            gn->start = (guint32)(ns < 0 ? 0 : ns);
            gn->pitch = (guint8)CLAMP((int)m_grp_pitch[i] - dp, 0, 127);
        }
        Commit();
        UnlockMain();
        return;
    }

    MidiNote *n = midi_clip_note(m_clip, (guint)m_drag_note);
    if (!n) {
        m_drag_mode = 0;
        UnlockMain();
        return;
    }
    double dt = (where.x - m_press_x) * m_tpx;
    if (m_drag_mode == 1) { // move
        long ns = (long)m_orig_start + (long)dt;
        n->start = SnapTick(ns < 0 ? 0.0 : (double)ns);
        int dp = (int)floor((where.y - m_press_y) / m_key_h + 0.5);
        n->pitch = (guint8)CLAMP((int)m_orig_pitch - dp, 0, 127);
    } else if (m_drag_mode == 2) { // resize
        long nl = (long)m_orig_len + (long)dt;
        int step = SnapStep() > 1 ? SnapStep() : 1;
        if (nl < step)
            nl = step;
        n->length = SnapTick((double)nl);
        if (n->length < (guint32)step)
            n->length = (guint32)step;
    }
    Commit();
    UnlockMain();
}

void MidiWindow::RollMouseUp(BView *v, BPoint where)
{
    if (m_sel_dragging) {
        m_sel_dragging = false;
        if (m_sel_moved) { // box drag: keep the selection, drop the outline
            m_sel_moved = false;
            m_roll->Invalidate();
            m_vel->Invalidate();
        } else { // plain right-click: context menu
            ShowRollContextMenu(v->ConvertToScreen(where), m_ctx_note_idx);
        }
        return;
    }
    m_drag_mode = 0;
    m_drag_note = -1;
}

void MidiWindow::RollWheel(float dy, int32 mods)
{
    if (mods & B_CONTROL_KEY) { // zoom
        double f = (dy > 0) ? 1.2 : 0.8;
        m_tpx = CLAMP(m_tpx * f, 0.5, 200.0);
    } else if (mods & B_SHIFT_KEY) { // horizontal scroll
        m_h_tick = CLAMP(m_h_tick + (double)dy * m_tpx * 60.0, 0.0, kHMaxTicks);
    } else { // vertical scroll
        double vis_rows = m_roll->Bounds().Height() / (double)m_key_h;
        double maxv = 128.0 - vis_rows;
        if (maxv < 0)
            maxv = 0;
        m_v_row = CLAMP(m_v_row + (double)dy * 3.0, 0.0, maxv);
    }
    UpdateScrollbars();
    RedrawAll();
}

void MidiWindow::ShowRollContextMenu(BPoint screen, int note_idx)
{
    m_ctx_note_idx = note_idx;
    LockMain();
    guint sc = SelCount();
    UnlockMain();

    BPopUpMenu menu("midi_context", false, false);
    BMenuItem *mi_del = new BMenuItem(sc > 0 ? "Delete Selected" : "Delete Note", NULL);
    mi_del->SetEnabled(note_idx >= 0 || sc > 0);
    menu.AddItem(mi_del);
    menu.AddSeparatorItem();
    BMenuItem *mi_copy = new BMenuItem("Copy  [Ctrl+C]", NULL);
    mi_copy->SetEnabled(sc > 0);
    menu.AddItem(mi_copy);
    BMenuItem *mi_paste = new BMenuItem("Paste  [Ctrl+V]", NULL);
    mi_paste->SetEnabled(!m_clipboard.empty());
    menu.AddItem(mi_paste);
    menu.AddSeparatorItem();
    BMenuItem *mi_selall = new BMenuItem("Select All", NULL);
    menu.AddItem(mi_selall);
    BMenuItem *mi_quant =
        new BMenuItem(sc > 0 ? "Quantize Selected  [Q]" : "Quantize All  [Q]", NULL);
    menu.AddItem(mi_quant);
    menu.AddSeparatorItem();
    BMenuItem *mi_clrloop = new BMenuItem("Clear Loop Region", NULL);
    mi_clrloop->SetEnabled(jackdaw_engine_has_loop_region());
    menu.AddItem(mi_clrloop);

    BMenuItem *chosen = menu.Go(screen, false, true);
    if (!chosen)
        return;
    if (chosen == mi_del)
        DeleteSelectedOrCtx();
    else if (chosen == mi_copy)
        CopySelection();
    else if (chosen == mi_paste)
        PasteAtPlayhead();
    else if (chosen == mi_selall)
        SelectAll();
    else if (chosen == mi_quant)
        Quantize();
    else if (chosen == mi_clrloop)
        ClearLoopRegion();
}

// ---- scrollbars -----------------------------------------------------------------

void MidiWindow::SetHTick(double tick)
{
    m_h_tick = CLAMP(tick, 0.0, kHMaxTicks);
    RedrawAll();
}

void MidiWindow::SetVRow(double row)
{
    m_v_row = CLAMP(row, 0.0, 128.0);
    m_roll->Invalidate();
    m_keys->Invalidate();
}

void MidiWindow::UpdateScrollbars()
{
    if (!m_hscroll || !m_vscroll || !m_roll)
        return;
    double vis_ticks = VisibleTicks();
    float hmax = (float)(kHMaxTicks - vis_ticks);
    if (hmax < 0)
        hmax = 0;
    m_hscroll->SetRange(0, hmax);
    m_hscroll->SetValue((float)m_h_tick);
    m_hscroll->SetProportion((float)(vis_ticks / kHMaxTicks));
    m_hscroll->SetSteps((float)(JACKDAW_PPQ / 4), (float)(vis_ticks * 0.9));

    double vis_rows = m_roll->Bounds().Height() / (double)m_key_h;
    float vmax = (float)(128.0 - vis_rows);
    if (vmax < 0)
        vmax = 0;
    m_vscroll->SetRange(0, vmax);
    m_vscroll->SetValue((float)m_v_row);
    m_vscroll->SetProportion((float)(vis_rows / 128.0));
    m_vscroll->SetSteps(1.0f, (float)vis_rows);
}

// ---- 50 ms tick -------------------------------------------------------------------

void MidiWindow::Tick()
{
    // Sync toggle visuals with the actual engine state (may be driven from the
    // main window or a footswitch).
    bool playing = jackdaw_engine_is_playing();
    int32 want = playing ? B_CONTROL_ON : B_CONTROL_OFF;
    if (m_btn_play->Value() != want)
        m_btn_play->SetValue(want);
    bool loop_on = jackdaw_engine_get_loop_enabled();
    want = loop_on ? B_CONTROL_ON : B_CONTROL_OFF;
    if (m_btn_loop->Value() != want)
        m_btn_loop->SetValue(want);

    // Refresh cached tempo/snap (project fields are main-looper-owned).
    LockMain();
    m_fpb = jackdaw_project_frames_per_beat(m_project, jackdaw_engine_get_sample_rate());
    m_bpb = m_project->beats_per_bar ? m_project->beats_per_bar : 4;
    m_chan = ClipDefaultChannel(m_clip); // recording may have added content
    bool snap_on = m_project->snap_enabled;
    UnlockMain();
    want = snap_on ? B_CONTROL_ON : B_CONTROL_OFF;
    if (m_btn_snap->Value() != want)
        m_btn_snap->SetValue(want);

    if (!jackdaw_engine_is_running())
        return;

    // Time display + playhead tick.
    off_t pos = jackdaw_engine_get_play_pos();
    char tbuf[64];
    format_timecode(jackdaw_engine_get_sample_rate(), pos, 0, tbuf, TIMEMODE_REAL);
    m_time_label->SetText(tbuf);
    m_play_tick = (m_fpb > 1.0) ? (double)pos / m_fpb * (double)JACKDAW_PPQ : -1.0;

    // Auto-scroll: page the view when the playhead passes the right edge.
    if (playing && m_play_tick >= 0.0 && pos != m_prev_play_pos) {
        m_prev_play_pos = pos;
        double vis_ticks = VisibleTicks();
        if (m_play_tick > m_h_tick + vis_ticks) {
            double nv = m_play_tick - 0.10 * vis_ticks;
            if (nv < 0)
                nv = 0;
            m_h_tick = nv;
            UpdateScrollbars();
            RedrawAll();
            return;
        }
    }

    m_roll->Invalidate();
    m_ruler->Invalidate();
}

// ---- window messages ----------------------------------------------------------

void MidiWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_MW_TICK:
            Tick();
            break;
        case MSG_MW_PLAY:
            // The toggle already flipped; drive engine state to match.
            m_main_msgr.SendMessage(m_btn_play->Value() == B_CONTROL_ON ? MSG_TRANSPORT_PLAY
                                                                        : MSG_TRANSPORT_PAUSE);
            break;
        case MSG_MW_LOOP:
            m_main_msgr.SendMessage(MSG_TRANSPORT_LOOP);
            break;
        case MSG_MW_PAUSE:
        case MSG_MW_STOP:
            m_main_msgr.SendMessage(MSG_TRANSPORT_PAUSE);
            break;
        case MSG_MW_RTZ:
            LocateStart();
            break;
        case MSG_MW_STEP_BACK:
            m_main_msgr.SendMessage(MSG_TRANSPORT_STEP_BACK);
            break;
        case MSG_MW_STEP_FWD:
            m_main_msgr.SendMessage(MSG_TRANSPORT_STEP_FWD);
            break;
        case MSG_MW_NEXT:
            m_main_msgr.SendMessage(MSG_TRANSPORT_NEXT_BOUNDARY);
            break;
        case MSG_MW_SNAP:
            LockMain();
            jackdaw_project_set_snap_enabled(m_project, m_btn_snap->Value() == B_CONTROL_ON);
            UnlockMain();
            break;
        case MSG_MIDI_EDITOR_PRESENT:
            Activate();
            break;
        case MSG_MIDI_EDITOR_REFRESH:
            RefreshAfterUndo();
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}
