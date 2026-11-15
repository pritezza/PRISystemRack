#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"
#include "WheelSteps.h"

// 整数の値を表示し、マウスで変える箱（処理バッファの ms など）。
// - クリックしたまま上下に動かす: 上で増え、下で減る（dragPixelsPerStep ピクセルで 1）。動かしている間はポインタが画面の端で止まらない
// - 動かさずにクリック: 入力欄になる（Enter で決定、Esc で取り消し、ほかをクリックしても決定）
// - マウスホイール: 1 目盛りで 1
// 無効（setEnabled (false)）のときは表示だけ
class NumberBox final : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    std::function<void (int)> onValueChange; // マウスや入力で変わったとき（setValue では呼ばない）

    NumberBox()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setWantsKeyboardFocus (false);
    }

    void setRange (int minimumValue, int maximumValue)
    {
        minimum = minimumValue;
        maximum = maximumValue;
        setValue (value);
    }

    void setSuffix (const juce::String& newSuffix)
    {
        suffix = newSuffix;
        repaint();
    }

    int getValue() const { return value; }

    void setValue (int newValue)
    {
        newValue = juce::jlimit (minimum, maximum, newValue);
        if (newValue != value)
        {
            value = newValue;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Theme::card);
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (dragging ? Theme::accent : Theme::buttonBorder);
        g.drawRoundedRectangle (area, 4.0f, 1.0f);

        if (editor != nullptr)
            return; // 入力中は入力欄が上に乗る

        g.setColour (isEnabled() ? Theme::text : Theme::dimText);
        g.setFont (juce::FontOptions (Theme::bodySize));
        g.drawText (juce::String (value) + suffix, getLocalBounds().reduced (6, 0), juce::Justification::centred, false);
    }

    void enablementChanged() override
    {
        setMouseCursor (isEnabled() ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || editor != nullptr)
            return;

        dragStartValue = value;
        dragged = false;
        e.source.enableUnboundedMouseMovement (true); // 画面の端で止まらずに動かせる
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || editor != nullptr)
            return;

        const int dy = -e.getDistanceFromDragStartY(); // 上に動かすと正
        if (! dragged && std::abs (dy) < dragPixelsPerStep)
            return;

        if (! dragged)
        {
            dragged = true;
            dragging = true;
            repaint();
        }

        changeTo (dragStartValue + dy / dragPixelsPerStep);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || editor != nullptr)
            return;

        e.source.enableUnboundedMouseMovement (false);
        if (dragging)
        {
            dragging = false;
            repaint();
        }

        if (! dragged && e.mouseWasClicked())
            showEditor();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (! isEnabled() || editor != nullptr)
            return;

        const int steps = wheelSteps.take (WheelSteps::verticalOrHorizontal (wheel));
        if (steps == 0)
            return;
        changeTo (value + steps);
    }

    void resized() override
    {
        if (editor != nullptr)
            editor->setBounds (getLocalBounds());
    }

private:
    static constexpr int dragPixelsPerStep = 3;

    void changeTo (int newValue)
    {
        newValue = juce::jlimit (minimum, maximum, newValue);
        if (newValue == value)
            return;

        value = newValue;
        repaint();
        if (onValueChange != nullptr)
            onValueChange (value);
    }

    void showEditor()
    {
        editor = std::make_unique<juce::TextEditor>();
        editor->setFont (juce::FontOptions (Theme::bodySize));
        editor->setJustification (juce::Justification::centred);
        editor->setInputRestrictions (3, "0123456789");
        editor->setIndents (2, 0);
        editor->setText (juce::String (value), false);
        editor->onReturnKey = [this] { finishEditing (true); };
        editor->onEscapeKey = [this] { finishEditing (false); };
        editor->onFocusLost = [this] { finishEditing (true); };
        addAndMakeVisible (*editor);
        editor->setBounds (getLocalBounds());
        editor->selectAll();
        editor->grabKeyboardFocus();
        repaint();
    }

    void finishEditing (bool apply)
    {
        if (editor == nullptr)
            return;

        const auto text = editor->getText().trim();
        // 入力欄はこの呼び出しの中から消さない（入力欄の処理が終わってから消す）
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NumberBox> (this)]
        {
            if (safe != nullptr)
            {
                safe->editor.reset();
                safe->repaint();
            }
        });
        editor->onFocusLost = nullptr;

        if (apply && text.isNotEmpty())
            changeTo (text.getIntValue());
    }

    int minimum = 0, maximum = 100, value = 0;
    juce::String suffix;
    int dragStartValue = 0;
    bool dragged = false, dragging = false;
    WheelSteps wheelSteps;
    std::unique_ptr<juce::TextEditor> editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NumberBox)
};
