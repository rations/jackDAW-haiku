#pragma once

#include <Invoker.h>
#include <View.h>

// Rotary dial (a port of the Linux JackDAW knob widget). Vertical drag adjusts
// the value (150 px = full range), double-click resets to the default, the
// scroll wheel steps it, and a one-letter identifier ("V"/"P") is drawn in the
// dial face so the host needs no separate label. On every change it Invokes its
// message with a float "value" attached; the owner sets the target and reads it
// back. Formatting follows the kind: dB, pan (L/C/R), or a plain number.
class KnobView : public BView, public BInvoker
{
public:
    enum Kind { KIND_DB, KIND_PAN, KIND_PLAIN };

    KnobView(const char *name, double min_value, double max_value, double value,
             double default_value, Kind kind, BMessage *message);

    void AttachedToWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override;
    void MessageReceived(BMessage *message) override;

    double Value() const
    {
        return m_value;
    }
    // Set without notifying (used to sync from engine state).
    void SetValue(double value);
    void SetCenterLabel(const char *label);

private:
    void FormatValue(char *buf, size_t len) const;
    void ApplyDelta(double new_value); // clamp, redraw, notify

    double m_value;
    double m_min;
    double m_max;
    double m_default;
    Kind m_kind;
    char m_center[4];

    bool m_dragging;
    float m_drag_start_y;
    double m_drag_start_val;
};
