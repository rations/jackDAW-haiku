#ifndef MIDI_WINDOW_H
#define MIDI_WINDOW_H

#include <Messenger.h>
#include <Window.h>

#include <vector>

#include "engine/project.h"
#include "engine/track.h"

class BButton;
class BMessageRunner;
class BScrollBar;
class BStringView;
class MidiRollView;
class MidiKeysView;
class MidiVelView;
class MidiRollRulerView;

// Piano-roll MIDI editor — a per-track top-level window. Layout:
//   [transport toolbar — matches the main window style, no recording]
//   [ keyboard | ruler (bar numbers + playhead + loop tabs) ][         ]
//   [ keyboard | note grid (+ playhead)                     ][v-scroll ]
//   [          | velocity lane                              ]
//   [          | horizontal scrollbar                       ]
// Notes are stored in TICKS (JACKDAW_PPQ per quarter); after any edit the RT
// snapshot is republished via jackdaw_track_commit_midi().
//
// Threading plan (this window runs its own looper thread):
//  - The MIDI clip, undo manager and project fields are owned by the main
//    window's looper. Every read or write of them from here happens while
//    holding the MAIN window's looper lock (LockMain/UnlockMain), which
//    serializes against the timeline draw, record finalize and undo.
//  - Engine transport/state MUTATION is routed to the main window as
//    BMessages (MSG_TRANSPORT_*, MSG_MIDI_LOCATE, MSG_MIDI_SET_LOOP,
//    MSG_MIDI_PREVIEW); only lock-free atomic GETTERS (play position,
//    playing/loop state, sample rate) are called directly.
//  - The main window must NEVER lock a MidiWindow (async PostMessage only),
//    so the main-lock acquisition here cannot form a lock-order cycle.
class MidiWindow : public BWindow
{
public:
    // Construct + wire on the MAIN window's looper only (OpenMidiEditor).
    // Takes its own GObject ref on `track`.
    MidiWindow(JackDawTrack *track, JackDawProject *project, BWindow *main);
    virtual ~MidiWindow();

    void MessageReceived(BMessage *message) override;
    bool QuitRequested() override;

    JackDawTrack *Track() const
    {
        return m_track;
    }

    // ---- called by the child views (this window's looper) ----
    void RollDraw(BView *v, BRect bounds);
    void RollMouseDown(BView *v, BPoint where);
    void RollMouseMoved(BView *v, BPoint where);
    void RollMouseUp(BView *v, BPoint where);
    void RollWheel(float dy, int32 mods);
    void KeysDraw(BView *v, BRect bounds);
    void KeysMouseDown(BView *v, BPoint where);
    void KeysMouseMoved(BView *v, BPoint where);
    void KeysMouseUp(BView *v, BPoint where);
    void VelDraw(BView *v, BRect bounds);
    void VelMouseDown(BView *v, BPoint where);
    void VelMouseMoved(BView *v, BPoint where);
    void VelMouseUp(BView *v, BPoint where);
    void RulerDraw(BView *v, BRect bounds);
    void RulerMouseDown(BView *v, BPoint where);
    void RulerMouseMoved(BView *v, BPoint where);
    void RulerMouseUp(BView *v, BPoint where);
    void SetHTick(double tick); // horizontal scrollbar drive
    void SetVRow(double row);   // vertical scrollbar drive
    void UpdateScrollbars();

    // ---- keyboard shortcuts (message filter on this window) ----
    void SelectAll();
    void ClearSelection();
    void Quantize();
    void CopySelection();
    void PasteAtPlayhead();
    void LocateStart();

private:
    // Main-window looper lock guard (see the threading plan above).
    void LockMain()
    {
        m_main->Lock();
    }
    void UnlockMain()
    {
        m_main->Unlock();
    }

    // Coordinate mapping (mirrors the Linux editor's helpers).
    int SnapStep() const;
    guint32 SnapTick(double t) const;
    double TickToX(double tick) const
    {
        return (tick - m_h_tick) / m_tpx;
    }
    double XToTick(double x) const;
    double PitchToY(int pitch) const
    {
        return ((127 - pitch) - m_v_row) * m_key_h;
    }
    int YToPitch(double y) const;
    double VisibleTicks() const;

    void Commit(); // republish RT snapshot + repaint (main-locked)
    void PushUndo(const char *desc);
    int NoteAt(double x, double y, bool *on_edge) const;

    void SelEnsure();
    bool SelIs(guint i) const;
    guint SelCount() const;
    void GrpCapture();
    void SelUpdateBox();

    void DeleteSelectedOrCtx();
    void ClearLoopRegion();
    void ShowRollContextMenu(BPoint screen, int note_idx);

    void KeysPlay(int pitch);
    void KeysStop();
    int VelNoteAtX(double x) const;
    void VelApplyY(int note_idx, double y);

    void RulerSeekToX(double x);
    int RulerLoopHit(double x) const;
    void RulerLoopDragTo(double x);

    void Tick(); // 50 ms: playhead, auto-scroll, button/time sync
    void RedrawAll();
    void RefreshAfterUndo();

    JackDawTrack *m_track;     // strong GObject ref
    JackDawProject *m_project; // weak (owned by the app)
    MidiClip *m_clip;          // cached track->midi_clip; refreshed after undo
    BWindow *m_main;           // main window (outlives every MidiWindow)
    BMessenger m_main_msgr;

    MidiRollView *m_roll;
    MidiKeysView *m_keys;
    MidiVelView *m_vel;
    MidiRollRulerView *m_ruler;
    BScrollBar *m_hscroll;
    BScrollBar *m_vscroll;
    BButton *m_btn_play;
    BButton *m_btn_loop;
    BButton *m_btn_snap;
    BStringView *m_time_label;
    BMessageRunner *m_runner;

    double m_tpx;    // ticks per pixel (zoom)
    int m_key_h;     // px per semitone row
    double m_h_tick; // leftmost visible tick
    double m_v_row;  // topmost visible row (0 = pitch 127)

    // Cached per-tick project values so Draw() stays consistent between locks.
    double m_fpb; // frames per beat
    guint m_bpb;  // beats per bar

    // Interaction state (mirrors the Linux editor).
    int m_drag_mode; // 0 none, 1 move, 2 resize, 3 velocity, 4 group move
    int m_drag_note;
    double m_press_x, m_press_y;
    guint32 m_orig_start, m_orig_len;
    guint8 m_orig_pitch;
    int m_ctx_note_idx;

    std::vector<guint8> m_sel; // parallel to clip->notes by index
    std::vector<guint32> m_grp_start;
    std::vector<guint8> m_grp_pitch;
    std::vector<guint8> m_grp_vel;
    std::vector<MidiNote> m_clipboard; // starts normalized to tick 0

    bool m_sel_dragging; // right-drag rubber band on the roll
    bool m_sel_moved;
    double m_sel_x0, m_sel_y0, m_sel_x1, m_sel_y1;

    int m_hl_pitch;    // highlighted lane pitch, -1 = none
    int m_key_playing; // pitch auditioned via the keyboard, -1 = none

    double m_play_tick; // playhead in ticks, -1 = off
    off_t m_prev_play_pos;
    bool m_ruler_dragging;
    int m_loop_drag_edge; // 0 none, 1 start tab, 2 end tab
};

#endif // MIDI_WINDOW_H
