#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// 中身の部品を倍率どおりに拡大して表示する入れ物（ウィンドウの中身に使う）。
// 中身は「論理的な大きさ」（ウィンドウの大きさ ÷ 倍率）で配置され、描画とマウスの座標は JUCE が変換する。
// 文字の大きさの数値ではなく描画ごと拡大するので、レイアウトを崩さずに全体を大きくできる
class ScaledContent final : public juce::Component
{
public:
    ScaledContent (std::unique_ptr<juce::Component> contentToShow, float initialScale)
        : content (std::move (contentToShow)), scale (initialScale)
    {
        addAndMakeVisible (*content);
        setSize (juce::roundToInt ((float) content->getWidth() * scale), juce::roundToInt ((float) content->getHeight() * scale));
    }

    juce::Component* getContent() const { return content.get(); }
    float getScale() const { return scale; }

    // 倍率を変え、中身の論理的な大きさを保つようにウィンドウの大きさも変える（画面の 9 割まで）。
    // minLogicalWidth / minLogicalHeight: ウィンドウの最小の大きさ（倍率をかける前）
    void rescaleWindow (juce::ResizableWindow& window, float newScale, int minLogicalWidth, int minLogicalHeight)
    {
        if (newScale == scale)
            return;

        const float logicalWidth = (float) getWidth() / scale;
        const float logicalHeight = (float) getHeight() / scale;
        scale = newScale;

        window.setResizeLimits (juce::roundToInt ((float) minLogicalWidth * scale), juce::roundToInt ((float) minLogicalHeight * scale), 8000, 6000);

        // ウィンドウの枠（ネイティブのタイトルバーは含まない）の分を足して、中身がこの大きさになるようにする
        const int frameWidth = window.getWidth() - getWidth();
        const int frameHeight = window.getHeight() - getHeight();
        window.setSize (juce::roundToInt (logicalWidth * scale) + frameWidth, juce::roundToInt (logicalHeight * scale) + frameHeight);
        fitToScreen (window);
        resized();
        repaint();
    }

    // 最初に開くとき、画面からはみ出さない大きさにする（作業領域の 9 割まで）
    static void fitToScreen (juce::Component& window)
    {
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = display->userBounds.toNearestInt();
            window.setSize (juce::jmin (window.getWidth(), area.getWidth() * 9 / 10),
                            juce::jmin (window.getHeight(), area.getHeight() * 9 / 10));
        }
    }

    void resized() override
    {
        content->setTransform (juce::AffineTransform::scale (scale));
        content->setBounds (0, 0, juce::roundToInt ((float) getWidth() / scale), juce::roundToInt ((float) getHeight() / scale));
    }

private:
    std::unique_ptr<juce::Component> content;
    float scale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaledContent)
};
