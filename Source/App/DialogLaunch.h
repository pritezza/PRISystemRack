#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// juce::DialogWindow::LaunchOptions::launchAsync の代わり（できるものは同じ）。
//
// JUCE のものは、ウィンドウを作ってから setResizable と setUsingNativeTitleBar を呼ぶので、そのたびにウィンドウを作り直す
// （TopLevelWindow::recreateDesktopWindow）。作り直すたびに、まだ見えないウィンドウを toFront (true) で手前に出して
// アクティブにするので、開くまでの間にメイン画面のタイトルバーが何度も「手前 / 手前でない」の色に変わっていた
// （2026-11-12、ウィンドウの手前の移り変わりを記録して確認。1 つのダイアログで 3 回作っていた）。
// ここでは、ウィンドウを作る前（addToDesktop の前）に設定を済ませ、1 回だけ作る
namespace DialogLaunch
{
class Window final : public juce::DialogWindow
{
public:
    explicit Window (juce::DialogWindow::LaunchOptions& options)
        : DialogWindow (options.dialogTitle, options.dialogBackgroundColour, options.escapeKeyTriggersCloseButton,
                        false, // まだウィンドウを作らない
                        options.componentToCentreAround != nullptr
                            ? juce::Component::getApproximateScaleFactorForComponent (options.componentToCentreAround)
                            : 1.0f)
    {
        // 枠の太さ（タイトルバーの種類で変わる）が決まってから中身の大きさに合わせるよう、先にタイトルバーを決める
        setUsingNativeTitleBar (options.useNativeTitleBar);

        if (options.content.willDeleteObject())
            setContentOwned (options.content.release(), true);
        else
            setContentNonOwned (options.content.release(), true);

        centreAroundComponent (options.componentToCentreAround, getWidth(), getHeight());
        setResizable (options.resizable, options.useBottomRightCornerResizer);

        addToDesktop(); // ここで 1 回だけ作る（まだ表示しない）
        setAlwaysOnTop (juce::WindowUtils::areThereAnyAlwaysOnTopWindows()); // Windows では作り直さずに切り替わる
    }

    void closeButtonPressed() override
    {
        if (canClose != nullptr && ! canClose())
            return; // 今は閉じられない（プラグインを読み込んでいる最中など）
        setVisible (false); // モーダルが終わり、消える（enterModalState で消すように頼んである）
    }

    // Esc（escapeKeyTriggersCloseButton のとき）も、× と同じく canClose に従う（JUCE の既定は確かめずに隠す）
    bool escapeKeyPressed() override
    {
        if (canClose != nullptr && ! canClose())
            return true; // 押されたことにして、ほかには渡さない
        return juce::DialogWindow::escapeKeyPressed();
    }

    // 閉じてよいかを返す（設定しなければいつでも閉じられる）。× と Esc と Alt+F4 で確かめる
    std::function<bool()> canClose;
};

// 作って、表示して、モーダルにする（閉じると消える）。JUCE の launchAsync と同じく、作ったウィンドウを返す
inline juce::DialogWindow* launchAsync (juce::DialogWindow::LaunchOptions& options)
{
    jassert (options.content != nullptr);
    auto* window = new Window (options);
    window->enterModalState (true, nullptr, true);

    // JUCE の setVisible はアクティブにしない（SW_SHOWNA）。JUCE の launchAsync では、作り直しのたびの toFront (true) で
    // 結果としてアクティブになっていたので、ここで 1 回だけ手前に出してアクティブにする（しないとタイトルバーが手前でない色のまま）
    window->toFront (true);
    return window;
}
} // namespace DialogLaunch
