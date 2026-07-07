#include "StepperControl.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <TextControl.h>
#include <TextView.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Internal messages: the field and the two buttons target this view; it then
// Invokes the model message outward to the real target.
enum {
    MSG_STEP_TEXT = 'sctx',
    MSG_STEP_MINUS = 'scmn',
    MSG_STEP_PLUS = 'scpl',
};

StepperControl::StepperControl(const char *name, const char *label, BMessage *message,
                               double min_value, double max_value, double step, int decimals)
    : BView(name, B_WILL_DRAW), BInvoker(message, NULL), m_text(NULL), m_minus(NULL), m_plus(NULL),
      m_value(min_value), m_min(min_value), m_max(max_value), m_step(step), m_decimals(decimals)
{
    m_text = new BTextControl(name, label, "", new BMessage(MSG_STEP_TEXT));
    m_minus = new BButton("minus", "−", new BMessage(MSG_STEP_MINUS));
    m_plus = new BButton("plus", "+", new BMessage(MSG_STEP_PLUS));

    BLayoutBuilder::Group<>(this, B_HORIZONTAL, 2.0f).Add(m_text).Add(m_minus).Add(m_plus);
}

void StepperControl::AttachedToWindow()
{
    BView::AttachedToWindow();
    m_text->SetTarget(this);
    m_minus->SetTarget(this);
    m_plus->SetTarget(this);

    // Size everything now that the fonts are valid: the label sits left of a
    // field wide enough for the biggest value, the −/+ buttons are compact, and
    // the whole control is pinned so the parent row can't stretch it and fling
    // the buttons apart.
    const char *label = m_text->Label();
    float label_w = (label && *label) ? m_text->StringWidth(label) + 10.0f : 0.0f;
    float field_w = m_text->StringWidth("0000") + 16.0f;
    float text_w = label_w + field_w;
    if (label_w > 0.0f)
        m_text->SetDivider(label_w);
    m_text->SetExplicitMinSize(BSize(text_w, B_SIZE_UNSET));
    m_text->SetExplicitMaxSize(BSize(text_w, B_SIZE_UNSET));

    float btn_w = m_plus->StringWidth("+") + 18.0f;
    m_minus->SetExplicitMinSize(BSize(btn_w, B_SIZE_UNSET));
    m_minus->SetExplicitMaxSize(BSize(btn_w, B_SIZE_UNSET));
    m_plus->SetExplicitMinSize(BSize(btn_w, B_SIZE_UNSET));
    m_plus->SetExplicitMaxSize(BSize(btn_w, B_SIZE_UNSET));

    float total = text_w + 2.0f * btn_w + 6.0f;
    SetExplicitMinSize(BSize(total, B_SIZE_UNSET));
    SetExplicitMaxSize(BSize(total, B_SIZE_UNSET));

    SetValue(m_value);
}

void StepperControl::Format(char *buf, size_t len, double value) const
{
    snprintf(buf, len, "%.*f", m_decimals, value);
}

double StepperControl::Value() const
{
    return m_value;
}

void StepperControl::SetValue(double value)
{
    if (value < m_min)
        value = m_min;
    if (value > m_max)
        value = m_max;
    m_value = value;

    char buf[32];
    Format(buf, sizeof(buf), m_value);
    m_text->SetText(buf);
}

void StepperControl::CommitFromText()
{
    SetValue(atof(m_text->Text())); // clamp + reformat the possibly-garbage entry

    // Drop keyboard focus so window transport keys work again, and clear the
    // lingering select-all the control leaves behind on Enter.
    m_text->MakeFocus(false);
    m_text->TextView()->Select(0, 0);

    BMessage out(*Message());
    out.AddFloat("value", (float)m_value);
    Invoke(&out);
}

void StepperControl::Nudge(double delta)
{
    SetValue(m_value + delta);
    BMessage out(*Message());
    out.AddFloat("value", (float)m_value);
    Invoke(&out);
}

void StepperControl::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case MSG_STEP_TEXT:
            CommitFromText();
            break;
        case MSG_STEP_MINUS:
            Nudge(-m_step);
            break;
        case MSG_STEP_PLUS:
            Nudge(+m_step);
            break;
        default:
            BView::MessageReceived(message);
            break;
    }
}
