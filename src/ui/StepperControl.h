#pragma once

#include <Invoker.h>
#include <View.h>

class BButton;
class BTextControl;

// A numeric stepper: a BTextControl flanked by "−" / "+" buttons. The Interface
// Kit has no native spin control, so this composes one — the BPM and
// time-signature entries in the transport bar.
//
// Behaves like a BControl: give it a model message and SetTarget(); it Invokes
// that message (with a "value" float attached) whenever the value changes, on
// Enter in the field or on a −/+ click. The value is clamped to [min, max] and
// snapped to the step; the handler can also read it back with Value().
class StepperControl : public BView, public BInvoker
{
public:
    StepperControl(const char *name, const char *label, BMessage *message, double min_value,
                   double max_value, double step, int decimals);

    void AttachedToWindow() override;
    void MessageReceived(BMessage *message) override;

    double Value() const;
    void SetValue(double value); // clamps + reformats, does not Invoke

private:
    void CommitFromText(); // parse the field, clamp, reformat, Invoke
    void Nudge(double delta);
    void Format(char *buf, size_t len, double value) const;

    BTextControl *m_text;
    BButton *m_minus;
    BButton *m_plus;

    double m_value;
    double m_min;
    double m_max;
    double m_step;
    int m_decimals;
};
