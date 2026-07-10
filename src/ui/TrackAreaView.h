#pragma once

#include <Messenger.h>
#include <View.h>

#include <map>
#include <vector>

#include "engine/clipregion.h"
#include "engine/project.h"

class TimelineView;
class TrackStripView;

// The area below the ruler: a vertical stack of per-track rows. Each row is a
// TrackStripView (left, in the 235 px strip column) plus a timeline lane drawn
// by this view (right of the column). Empty space beneath the last row keeps
// the click-to-locate behaviour. Strips are real child BViews (interactive
// controls); lanes have no widgets yet (clips arrive in a later phase) so this
// view draws them directly. Vertical scrolling is a pixel offset driven by the
// timeline's vertical scrollbar. Runs on the MainWindow looper thread.
class TrackAreaView : public BView
{
public:
    explicit TrackAreaView(TimelineView *timeline);
    ~TrackAreaView() override;

    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override;
    void MessageReceived(BMessage *message) override;
    void FrameResized(float w, float h) override;

    // Rebuild the strip children from the project's current track list.
    void RebuildStrips();
    // Total pixel height of all rows (for the vertical scrollbar range).
    float ContentHeight() const;
    void SetScrollY(float y);
    float ScrollY() const
    {
        return m_scroll_y;
    }
    // Push fresh peak values into each strip's VU (called on the UI tick).
    void UpdateMeters();

    // ---- Region editing (audio + instrument tracks) ----
    // A "section" is a ClipRegion* on an audio track or a MidiRegion* on an
    // instrument track; selection/move/split/copy machinery is shared across
    // both via the Sec* dispatch helpers below (same generalization as the
    // Linux timeline). Gain/group stay audio-only. Called from MainWindow
    // message routing (keyboard/menu) and the context menu. All run on this
    // looper — the single project mutator.
    void SplitAtCursor();            // split the active track's region at the play head
    void DeleteSelection();          // delete the selected sections, else the range
    void CopySelection();            // sections, else the range, to the clipboard
    void PasteAtCursor();            // clipboard onto the active track at the play head
    void GroupSelection();           // merge adjacent same-clip selected sections
    void SetSelectionGain(float db); // apply gain to selected sections / range
    void ClearSectionSelection();    // drop stale section pointers (after undo/redo)
    bool CanPaste() const;

    // Drop-insertion indicator for the track drag-reorder gesture. gap is the
    // boundary index in [0, track count] where the dragged row would land, or -1
    // to clear the indicator. Called by a TrackStripView mid-drag.
    void SetDropGap(int gap);

private:
    void RepositionStrips();
    int RowAtY(float y) const; // track index under a lane y, or -1
    // Draw every track's clip-region waveforms + region boundaries into the lane.
    void DrawWaveforms(BRect lane);
    // Live red waveform for tracks currently capturing a take (P7).
    void DrawRecordOverlay(BRect lane);
    // Instrument-track MIDI note rectangles + the live red note overlay (P8).
    void DrawMidiNotes(BRect lane);
    void DrawMidiRecOverlay(BRect lane);

    // ---- Region-edit helpers ----
    JackDawTrack *TrackAtRow(int row) const;
    off_t LaneXToFrame(float x) const; // lane x (incl. header offset) -> snapped frame
    // Kind-aware section dispatch (audio ClipRegion* / instrument MidiRegion*).
    double FramesPerTick() const;                      // MIDI tick->frame factor, 0 = unknown
    GPtrArray *SectionList(JackDawTrack *t) const;     // the track's live section list
    GPtrArray *SectionListCopy(JackDawTrack *t) const; // deep copy (move-undo memento)
    off_t SecTlPos(JackDawTrack *t, void *s) const;
    void SecSetTlPos(JackDawTrack *t, void *s, off_t pos);
    off_t SecEnd(JackDawTrack *t, void *s) const;
    void *SecListAt(JackDawTrack *t, off_t frame) const; // section covering frame, or NULL
    void PushRegionUndo(JackDawTrack *t);                // kind-aware project undo memento
    bool SectionSelContains(void *s) const;
    void SelectRegionAt(JackDawTrack *t, off_t frame); // select the section covering frame
    off_t SnapMoveDelta(off_t raw_delta) const;        // snap a block-move delta
    void CommitTrack(JackDawTrack *t);                 // sort + republish RT snapshot
    void ShowContextMenu(JackDawTrack *t, off_t frame, BPoint screen_where);
    void ClearMovePre(); // free the pristine per-track move-undo copies
    void PushMoveUndo(); // combined multi-track move undo from m_move_pre

    TimelineView *m_timeline; // borrowed (parent)
    BMessenger m_main;        // MainWindow (for strip context/structural msgs)
    std::vector<TrackStripView *> m_strips;

    float m_scroll_y;
    int m_drop_gap; // reorder insertion boundary, or -1 when not dragging

    // ---- Section selection + editing state ----
    // Selected sections are ClipRegion*/MidiRegion* (per m_sel_track's kind)
    // borrowed from the track's live list; any list-mutating edit clears the
    // selection because the pointers go stale.
    JackDawTrack *m_sel_track; // owner of the selected sections, or NULL
    std::vector<void *> m_sel_regions;

    // Rubber-band range selection (used by delete/copy/gain when no section sel).
    bool m_selecting;    // extending a rubber band this drag
    bool m_range_active; // a committed range exists
    off_t m_range_start;
    off_t m_range_end;

    // Block move-drag (horizontal slide + vertical track relocate).
    bool m_move_armed;     // pressed on a selected section; may become a move
    bool m_moving;         // past the move threshold
    bool m_move_committed; // pristine state captured into m_move_pre
    float m_move_press_x;
    float m_move_press_y;
    JackDawTrack *m_move_src;
    std::vector<off_t> m_move_orig;                   // original tl_pos per selected section
    std::map<JackDawTrack *, GPtrArray *> m_move_pre; // pristine lists (combined undo)

    // Clipboards: normalized section copies (earliest at frame 0). The audio
    // and MIDI lists are separate; m_clipboard_midi says which one the last
    // Copy filled, and Paste only targets a track of that kind.
    GPtrArray *m_clipboard;      // ClipRegion* copies
    GPtrArray *m_midi_clipboard; // MidiRegion* copies
    bool m_clipboard_midi;

    // Last right-clicked target (context-menu ops on a bare region / track).
    JackDawTrack *m_menu_track;
    off_t m_menu_frame;

    // Per-track "state-changed" handlers (region edits) → repaint the lane.
    // Parallel to m_strips; reconnected on every RebuildStrips().
    std::vector<gulong> m_track_state_handlers;

    gulong m_added_handler;
    gulong m_removed_handler;
    gulong m_reordered_handler;
    gulong m_selection_handler;
};
