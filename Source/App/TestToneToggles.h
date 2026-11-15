#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "RackEngine.h"
#include "Theme.h"

// 音声テスト（440 Hz のサイン波、-20 dBFS）のチェックボックス: 「音声テスト  ☐ 入力  ☐ 出力」。
// 入力は入力ゲインの手前に、出力はプラグインのあと・出力ゲインの手前に足す（RackEngine::setTestToneOn）。
// メイン画面の上の欄のバッファの行に置く（アプリ用ラックのウィンドウには置かない）。
// 再生デバイスで有効でなければ鳴らないので押せなくする（0.25 秒ごとに見直す。ほかで止めたときもチェックを合わせる）。
// この部品を消すと（画面を閉じた、言語を変えたなど）止める。鳴らしたまま見えなくならないように
class TestToneToggles final : public juce::Component,
                              private juce::Timer
{
public:
    explicit TestToneToggles (RackEngine& modelToUse)
        : model (modelToUse)
    {
        label.setText (TRANS ("Audio test"), juce::dontSendNotification);
        label.setFont (juce::FontOptions (Theme::compactToggleTextSize)); // チェックボックスと同じ小さめの文字
        label.setColour (juce::Label::textColourId, Theme::text);
        label.setBorderSize ({ 0, 0, 0, 0 });
        addAndMakeVisible (label);

        setUp (inputToggle, false, TRANS ("Input"),
               TRANS ("Adds a 440 Hz sine wave (-20 dBFS) before the input gain, so you can hear it through the plugins. Click again to stop."));
        setUp (outputToggle, true, TRANS ("Output"),
               TRANS ("Adds a 440 Hz sine wave (-20 dBFS) after the plugins, before the output gain (it does not go through the plugins). Click again to stop."));

        refresh();
        startTimer (250);
    }

    ~TestToneToggles() override
    {
        model.stopTestTones();
    }

    int getIdealWidth() const
    {
        return labelWidth() + gap + Theme::toggleWidth (inputToggle) + gap + Theme::toggleWidth (outputToggle);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromLeft (labelWidth()));
        area.removeFromLeft (gap);
        inputToggle.setBounds (area.removeFromLeft (Theme::toggleWidth (inputToggle)));
        area.removeFromLeft (gap);
        outputToggle.setBounds (area.removeFromLeft (Theme::toggleWidth (outputToggle)));
    }

private:
    static constexpr int gap = 8;

    void setUp (juce::ToggleButton& toggle, bool output, const juce::String& text, const juce::String& tooltip)
    {
        toggle.setButtonText (text);
        toggle.setTooltip (tooltip);
        Theme::makeCompactToggle (toggle);
        toggle.onClick = [this, &toggle, output] { model.setTestToneOn (output, toggle.getToggleState()); };
        addAndMakeVisible (toggle);
    }

    int labelWidth() const { return juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::compactToggleTextSize), label.getText()) + 2; }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        const bool enabled = PluginChain::isDeviceEnabled();
        inputToggle.setToggleState (model.isTestToneOn (false), juce::dontSendNotification);
        outputToggle.setToggleState (model.isTestToneOn (true), juce::dontSendNotification);
        inputToggle.setEnabled (enabled);
        outputToggle.setEnabled (enabled);
        label.setAlpha (enabled ? 1.0f : 0.5f);
    }

    RackEngine& model;
    juce::Label label;
    juce::ToggleButton inputToggle, outputToggle;

    JUCE_DECLARE_NON_COPYABLE (TestToneToggles)
};
