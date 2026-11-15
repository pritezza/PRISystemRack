#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// アプリ全体の配色と文字の大きさ。背景とカード、枠線と仕切りの明るさの差を大きめにとっている
namespace Theme
{
// 背景
inline const juce::Colour windowBackground { 0xff272a30 }; // 上の欄
inline const juce::Colour rackBackground { 0xff141518 };   // ラック（縦リスト）の背景
inline const juce::Colour panel { 0xff1e2024 };            // 状態表示などの囲み

// カード（入力・出力・プラグインの行）
inline const juce::Colour card { 0xff31353d };
inline const juce::Colour cardRaised { 0xff3a3f48 }; // ドラッグ中の行
inline const juce::Colour cardBorder { 0xff646a76 };
inline const juce::Colour divider { 0xff5c616c };    // 上の欄とラックの境目など
inline const juce::Colour arrow { 0xff8d929c };

// 文字
inline const juce::Colour text { 0xfff3f4f6 };
inline const juce::Colour subText { 0xffcdd1d8 };
inline const juce::Colour dimText { 0xffaab0b9 };

// ボタン
inline const juce::Colour button { 0xff3d424b };
inline const juce::Colour buttonBorder { 0xff737a86 };
inline const juce::Colour on { 0xff1f84ad };
inline const juce::Colour off { 0xff3d424b };
inline const juce::Colour resting { 0xff5b5680 }; // 無音のため休止中（ON ボタンを "Resting" にしてこの色にする）

// 意味のある色
inline const juce::Colour accent { 0xff5ccbfa };
inline const juce::Colour ok { 0xff8fe3aa };
inline const juce::Colour warning { 0xffffc46e };
inline const juce::Colour error { 0xffff9a90 };
inline const juce::Colour bypassOn { 0xffc0761a };

// アプリ用ラック（紫）
inline const juce::Colour mergeFill { 0xff372f52 };
inline const juce::Colour mergeBorder { 0xff9584e0 };
inline const juce::Colour mergeText { 0xffe6deff };

// 見出しの札
inline const juce::Colour inputPill { 0xff2f7d45 };
inline const juce::Colour outputPill { 0xff2b62a8 };
inline const juce::Colour mergePill { 0xff5e4d9c };

// 表示の大きさ（画面全体の倍率。設定の displayScale）。ユーザーが選ぶ 100% は、内部では baseScale 倍で描く。
// ウィンドウの中身は ScaledContent で effectiveScale() に拡大し、確認の画面の文字は AppLookAndFeel が同じ倍率をかける
// （メニューは JUCE が開いた元の部品の倍率に合わせて拡大する）。
// メッセージスレッドだけで読み書きする
inline constexpr float baseScale = 1.25f;
inline float uiScale = 1.0f;
inline constexpr float uiScaleChoices[] = { 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f }; // 50〜150%、10% 刻み
inline float effectiveScale() { return uiScale * baseScale; }

// ウィンドウの最小の大きさ（倍率をかける前）
inline constexpr int minWindowWidth = 560;
inline constexpr int minWindowHeight = 400;

// 文字の大きさ（倍率をかける前）
// （2026-11-08 に全体を 1 割ほど小さくした。以前は 17 / 15 / 13 / 14、ボタン 15、ボタンの高さ 24）
inline constexpr float titleSize = 15.0f;
inline constexpr float bodySize = 13.5f;
inline constexpr float smallSize = 12.0f;
inline constexpr float popupTextSize = 13.0f; // メニューと選択欄の一覧（デバイス名が長いので本文より小さめ）

inline void styleButton (juce::TextButton& b)
{
    b.setColour (juce::TextButton::buttonColourId, button);
    b.setColour (juce::TextButton::textColourOffId, text);
    b.setColour (juce::ComboBox::outlineColourId, buttonBorder);
}

inline void styleToggle (juce::TextButton& b, juce::Colour onColour = on)
{
    styleButton (b);
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
}

// ボタンと選択欄の高さ、ボタンの文字の大きさ（高さに関係なくこの大きさ。余白は詰めめ）
inline constexpr int controlHeight = 22;
inline constexpr float buttonTextSize = 13.5f;

// チェックボックス。文字の大きさはボタンの属性 textSize（なければ本文の大きさ）で、チェックの四角は文字に合わせた大きさ。
// メイン画面の上の欄（自動、無音時に休止、音声テスト）は小さめ（compactToggleTextSize）にする
inline constexpr float compactToggleTextSize = smallSize;
inline constexpr float toggleTickX = 2.0f;   // チェックの四角の左
inline constexpr float toggleTextGap = 6.0f; // チェックの四角と文字のすき間

inline void makeCompactToggle (juce::ToggleButton& b) { b.getProperties().set ("textSize", compactToggleTextSize); }

inline float toggleTextSize (const juce::Button& b) { return (float) b.getProperties().getWithDefault ("textSize", bodySize); }
inline float toggleTickWidth (float textSize) { return textSize * 1.1f; }

// 文字の長さに合わせたチェックボックスの幅
inline int toggleWidth (const juce::Button& b)
{
    const float size = toggleTextSize (b);
    return juce::roundToInt (toggleTickX + toggleTickWidth (size) + toggleTextGap)
         + juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (size), b.getButtonText()) + 4;
}

// 文字の長さに合わせたボタンの幅（左右の余白 padding）
inline int buttonWidth (const juce::TextButton& b, int padding = 8)
{
    return juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (buttonTextSize), b.getButtonText()) + padding * 2;
}
} // namespace Theme

// 既定の LookAndFeel。ボタンやメニューの文字を大きくし、配色を Theme に合わせる（通知領域のメニュー用に派生させる: AppTray.h の TrayMenuLookAndFeel）
class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel()
    {
        using namespace Theme;
        setColour (juce::ResizableWindow::backgroundColourId, windowBackground);
        setColour (juce::DocumentWindow::backgroundColourId, windowBackground);
        setColour (juce::Label::textColourId, text);
        setColour (juce::TextButton::buttonColourId, button);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::ComboBox::backgroundColourId, card);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::outlineColourId, buttonBorder);
        setColour (juce::ComboBox::arrowColourId, subText);
        setColour (juce::PopupMenu::backgroundColourId, card);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, on);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextEditor::backgroundColourId, card);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::outlineColourId, buttonBorder);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::ToggleButton::tickDisabledColourId, dimText);
        setColour (juce::ListBox::backgroundColourId, rackBackground);
        setColour (juce::ScrollBar::thumbColourId, buttonBorder);
        setColour (juce::AlertWindow::backgroundColourId, windowBackground);
        setColour (juce::AlertWindow::textColourId, text);
        setColour (juce::AlertWindow::outlineColourId, buttonBorder);
        setColour (juce::TooltipWindow::backgroundColourId, card);
        setColour (juce::TooltipWindow::textColourId, text);
        setColour (juce::TooltipWindow::outlineColourId, buttonBorder);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::backgroundColourId, cardBorder);
    }

    // ボタンの文字は高さに関係なく一定（小さなボタンだけ縮める）。
    // ボタンの属性 textSize があればその大きさにする（上の欄の「＋ ラックを追加」をラックの札にそろえるときなど）
    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
    {
        const float size = (float) button.getProperties().getWithDefault ("textSize", Theme::buttonTextSize);
        return juce::FontOptions (juce::jmin (size, (float) buttonHeight * 0.75f)); // 高さ 18 でも 13.5 のまま
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return juce::FontOptions (juce::jmin (Theme::buttonTextSize, (float) box.getHeight() * 0.8f)); // 高さ 18 でも本文の大きさのまま
    }

    // メニュー（選択欄の一覧も）は、JUCE が開いた元の部品の倍率（ScaledContent の倍率）に合わせて拡大するので、ここでは倍率をかけない。
    // メニューを開くときは必ず元の部品を指定する（PopupMenu::Options::withTargetComponent）
    juce::Font getPopupMenuFont() override
    {
        return juce::FontOptions (Theme::popupTextSize);
    }

    // メニューの行の高さ（JUCE の既定は文字の高さの 1.3 倍で窮屈なので、最低 24 にする。選択欄の一覧は選択欄の高さになる）
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
        if (! isSeparator && standardMenuItemHeight <= 0)
            idealHeight = juce::jmax (idealHeight, 24);
    }

    // チェックボックス（JUCE の既定は文字の大きさ 15 固定なので、本文の大きさか属性 textSize に合わせる。Theme::toggleWidth と同じ寸法）
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down) override
    {
        const float fontSize = juce::jmin (Theme::toggleTextSize (button), (float) button.getHeight() * 0.75f);
        const float tickWidth = Theme::toggleTickWidth (fontSize);
        drawTickBox (g, button, Theme::toggleTickX, ((float) button.getHeight() - tickWidth) * 0.5f, tickWidth, tickWidth,
                     button.getToggleState(), button.isEnabled(), highlighted, down);

        g.setColour (button.findColour (juce::ToggleButton::textColourId));
        g.setFont (juce::FontOptions (fontSize));
        if (! button.isEnabled())
            g.setOpacity (0.5f);
        g.drawFittedText (button.getButtonText(),
                          button.getLocalBounds().withTrimmedLeft (juce::roundToInt (Theme::toggleTickX + tickWidth + Theme::toggleTextGap)).withTrimmedRight (2),
                          juce::Justification::centredLeft, 10);
    }

    // 大きさを指定していないラベル（スライダーの数値の欄、状態表示など）は JUCE の既定の 15 なので、本文の大きさにする
    juce::Font getLabelFont (juce::Label& label) override
    {
        const auto font = label.getFont();
        return std::abs (font.getHeight() - 15.0f) < 0.01f ? font.withHeight (Theme::bodySize) : font;
    }

    // メニューバー（メイン画面の上端。画面の中にあるので ScaledContent の倍率で拡大される）
    juce::Font getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&) override
    {
        return juce::FontOptions (Theme::popupTextSize);
    }

    int getMenuBarItemWidth (juce::MenuBarComponent& bar, int index, const juce::String& text) override
    {
        return juce::GlyphArrangement::getStringWidthInt (getMenuBarFont (bar, index, text), text) + 22;
    }

    void drawMenuBarBackground (juce::Graphics& g, int width, int height, bool, juce::MenuBarComponent&) override
    {
        g.fillAll (Theme::panel);
        g.setColour (Theme::divider);
        g.fillRect (0, height - 1, width, 1);
    }

    void drawMenuBarItem (juce::Graphics& g, int width, int height, int index, const juce::String& text,
                          bool isMouseOverItem, bool isMenuOpen, bool, juce::MenuBarComponent& bar) override
    {
        const auto area = juce::Rectangle<int> (width, height - 1); // 下端の線は残す
        if (isMenuOpen)
            g.setColour (Theme::on);
        else if (isMouseOverItem)
            g.setColour (Theme::button);
        else
            g.setColour (juce::Colours::transparentBlack);
        g.fillRect (area);

        g.setColour (isMenuOpen ? juce::Colours::white : Theme::text);
        g.setFont (getMenuBarFont (bar, index, text));
        g.drawText (text, area, juce::Justification::centred, false);
    }

    // 確認の画面は別のウィンドウになり ScaledContent で拡大されないので、ここで倍率をかける
    juce::Font getAlertWindowMessageFont() override { return juce::FontOptions (Theme::bodySize * Theme::effectiveScale()); }
    juce::Font getAlertWindowTitleFont() override { return juce::FontOptions (Theme::titleSize * Theme::effectiveScale(), juce::Font::bold); }
    juce::Font getAlertWindowFont() override { return juce::FontOptions (Theme::bodySize * Theme::effectiveScale()); }
    int getAlertWindowButtonHeight() override { return juce::roundToInt (26.0f * Theme::effectiveScale()); }
};
