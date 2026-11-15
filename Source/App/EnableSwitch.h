#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

// 選んでいる再生デバイスで PRISystemRack を使うかの切り替え: [ 有効 | 無効 ] の 2 つに分かれたボタン。
// 今の状態の側を色付きで押し込んだ見た目にし（状態の表示を兼ねる）、反対側を押すと onSwitchRequested を呼ぶ。
// 切り替えには管理者の許可（UAC）が要るので、Windows の決まりに合わせて、押せる側の文字の左に盾のマークを付ける
class EnableSwitch final : public juce::Component,
                           public juce::TooltipClient
{
public:
    std::function<void()> onSwitchRequested;

    // ツールチップ: 今の状態の側（説明）と、押せる側（押すと何が起きるか）
    juce::String currentTooltip, actionTooltip;

    void setState (bool isEnabled)
    {
        if (deviceEnabled != isEnabled)
        {
            deviceEnabled = isEnabled;
            repaint();
        }
    }

    // 2 つの区画を同じ幅にした、全体の幅（切り替えても並びがずれないように）
    int getIdealWidth() const
    {
        const juce::FontOptions font (Theme::buttonTextSize, juce::Font::bold); // 今の状態の側は太字（太字の方が幅が広い）
        const int textWidth = juce::jmax (juce::GlyphArrangement::getStringWidthInt (font, TRANS ("Enabled")),
                                          juce::GlyphArrangement::getStringWidthInt (font, TRANS ("Disabled")));
        return ((int) iconSize + gap + textWidth + 8 * 2) * 2;
    }

    void paint (juce::Graphics& g) override
    {
        const float alpha = isEnabled() ? 1.0f : 0.45f;
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        constexpr float corner = 4.0f;

        g.setColour (Theme::button.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (bounds, corner);

        for (int segment = 0; segment < 2; ++segment)
        {
            const bool isEnabledSegment = segment == 0;
            const bool current = isEnabledSegment == deviceEnabled;
            const auto area = segmentArea (segment).toFloat();

            // 今の状態の側: 有効なら青、無効なら灰で塗る。押せる側: マウスが乗ったら少し明るく
            if (current || (isEnabled() && hovered == segment))
            {
                const auto fill = current ? (isEnabledSegment ? Theme::on : Theme::cardBorder)
                                          : Theme::button.brighter (pressed ? 0.25f : 0.12f);
                juce::Path path;
                path.addRoundedRectangle (area.getX(), area.getY(), area.getWidth(), area.getHeight(), corner, corner,
                                          segment == 0, segment == 1, segment == 0, segment == 1);
                g.setColour (fill.withMultipliedAlpha (alpha));
                g.fillPath (path);
            }

            const juce::FontOptions font (Theme::buttonTextSize, current ? juce::Font::bold : juce::Font::plain);
            const auto text = isEnabledSegment ? TRANS ("Enabled") : TRANS ("Disabled");
            const int textWidth = juce::GlyphArrangement::getStringWidthInt (font, text);
            const bool showShield = ! current;
            const float contentWidth = (showShield ? iconSize + (float) gap : 0.0f) + (float) textWidth;
            float x = area.getCentreX() - contentWidth * 0.5f;

            if (showShield)
            {
                drawShield (g, { x, area.getCentreY() - iconSize * 0.5f, iconSize * 0.84f, iconSize }, alpha);
                x += iconSize + (float) gap;
            }

            g.setColour ((current ? juce::Colours::white : Theme::subText).withMultipliedAlpha (alpha));
            g.setFont (font);
            g.drawText (text, juce::Rectangle<float> (x, area.getY(), (float) textWidth + 2.0f, area.getHeight()),
                        juce::Justification::centredLeft, false);
        }

        // 外枠と区切り線
        g.setColour (Theme::buttonBorder.withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds, corner, 1.0f);
        g.fillRect ((float) segmentArea (1).getX() - 0.5f, bounds.getY(), 1.0f, bounds.getHeight());
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (segmentAt (e.position)); }
    void mouseEnter (const juce::MouseEvent& e) override { setHovered (segmentAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressed = isClickable (segmentAt (e.position));
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override { setHovered (segmentAt (e.position)); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool wasPressed = pressed;
        pressed = false;
        repaint();
        if (wasPressed && isClickable (segmentAt (e.position)) && onSwitchRequested != nullptr)
            onSwitchRequested();
    }

    juce::String getTooltip() override
    {
        const int segment = segmentAt (getMouseXYRelative().toFloat());
        if (segment < 0)
            return {};
        return isClickable (segment) || ! isEnabled() ? actionTooltip : currentTooltip;
    }

private:
    static constexpr float iconSize = 14.0f;
    static constexpr int gap = 5;

    bool deviceEnabled = false;
    int hovered = -1;
    bool pressed = false;

    juce::Rectangle<int> segmentArea (int segment) const
    {
        auto area = getLocalBounds();
        const int half = area.getWidth() / 2;
        return segment == 0 ? area.removeFromLeft (half) : area.withTrimmedLeft (half);
    }

    int segmentAt (juce::Point<float> p) const
    {
        if (! getLocalBounds().toFloat().contains (p))
            return -1;
        return p.x < (float) segmentArea (1).getX() ? 0 : 1;
    }

    // 押せるのは今の状態でない側だけ
    bool isClickable (int segment) const
    {
        return isEnabled() && segment >= 0 && (segment == 0) != deviceEnabled;
    }

    void setHovered (int segment)
    {
        if (hovered != segment)
        {
            hovered = segment;
            repaint();
        }
    }

    // 青と黄の 4 つに分かれた盾
    static void drawShield (juce::Graphics& g, juce::Rectangle<float> r, float alpha)
    {
        juce::Path shield;
        shield.startNewSubPath (r.getCentreX(), r.getY());
        shield.lineTo (r.getRight(), r.getY() + r.getHeight() * 0.16f);
        shield.lineTo (r.getRight(), r.getY() + r.getHeight() * 0.50f);
        shield.quadraticTo (r.getRight(), r.getY() + r.getHeight() * 0.84f, r.getCentreX(), r.getBottom());
        shield.quadraticTo (r.getX(), r.getY() + r.getHeight() * 0.84f, r.getX(), r.getY() + r.getHeight() * 0.50f);
        shield.lineTo (r.getX(), r.getY() + r.getHeight() * 0.16f);
        shield.closeSubPath();

        const juce::Colour blue = juce::Colour (0xff3b82d9).withMultipliedAlpha (alpha);
        const juce::Colour yellow = juce::Colour (0xfff2c230).withMultipliedAlpha (alpha);
        const float midX = r.getCentreX();
        const float midY = r.getY() + r.getHeight() * 0.48f;

        {
            const juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (shield);
            g.setColour (blue);
            g.fillRect (juce::Rectangle<float> (r.getX(), r.getY(), midX - r.getX(), midY - r.getY()));
            g.fillRect (juce::Rectangle<float> (midX, midY, r.getRight() - midX, r.getBottom() - midY));
            g.setColour (yellow);
            g.fillRect (juce::Rectangle<float> (midX, r.getY(), r.getRight() - midX, midY - r.getY()));
            g.fillRect (juce::Rectangle<float> (r.getX(), midY, midX - r.getX(), r.getBottom() - midY));
        }

        g.setColour (juce::Colours::black.withAlpha (0.35f * alpha));
        g.strokePath (shield, juce::PathStrokeType (0.8f));
    }
};
