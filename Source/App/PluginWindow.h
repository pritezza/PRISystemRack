#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

#include "AlwaysOnTop.h"
#include "EditorGuard.h"
#include "WindowCapture.h"

// プラグインの画面を表示する別ウィンドウ。
// savedBounds があればその位置で開く（画面の外なら真ん中）。大きさは、プラグインの画面が大きさを変えられるときだけ戻す。
// 動かしたり大きさを変えたりしたら onBoundsChanged で知らせる（呼び出し元がプラグインごとに保存する）。
// 開いている間はときどき（開いて 1.5 秒後、その後 5 秒ごと）と閉じる直前に画面を撮り、onSnapshot で渡す（行の表示に使う）
class PluginWindow final : public juce::DocumentWindow,
                           private juce::Timer
{
public:
    // onEditorFault: プラグインの画面の中で例外が起きた（EditorGuard が受け止めた）。呼び出し元がこの窓を消す
    PluginWindow (juce::AudioProcessorEditor* editor, const juce::String& title, std::optional<juce::Rectangle<int>> savedBounds,
                  std::function<void (juce::Rectangle<int>)> onBoundsChangedCallback, std::function<void()> onCloseRequested,
                  std::function<void (const juce::Image&)> onSnapshotCallback, std::function<void()> onEditorFault)
        : DocumentWindow (title, juce::Colours::darkgrey, DocumentWindow::closeButton,
                          false), // まだウィンドウを作らない（設定を済ませてから addToDesktop で 1 回だけ作る）
          onClose (std::move (onCloseRequested)),
          onSnapshot (std::move (onSnapshotCallback))
    {
        // ウィンドウを作ったあとに setUsingNativeTitleBar や setResizable を呼ぶと、JUCE はそのたびにウィンドウを作り直し、
        // まだ見えないウィンドウを手前に出す。開くまでにメイン画面のタイトルバーが何度も「手前 / 手前でない」の色に変わり、
        // プラグインの画面も作り直したウィンドウに付け直されていたので、作る前に設定する（DialogLaunch.h と同じ）
        setUsingNativeTitleBar (true);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);

        if (savedBounds.has_value() && isOnScreen (*savedBounds))
        {
            if (editor->isResizable() && savedBounds->getWidth() > 0 && savedBounds->getHeight() > 0)
                setSize (savedBounds->getWidth(), savedBounds->getHeight());
            setTopLeftPosition (savedBounds->getPosition());
        }
        else
        {
            centreWithSize (getWidth(), getHeight());
        }

        addToDesktop(); // ここで 1 回だけ作る（まだ表示しない）
        onBoundsChanged = std::move (onBoundsChangedCallback); // ここまでの位置決めでは知らせない
        AlwaysOnTop::applyTo (*this); // Windows では作り直さずに切り替わる

        // プラグインの画面の中で落ちてもアプリごと落ちないよう、子ウィンドウのメッセージ処理を包む。
        // 表示すると JUCE がプラグインの画面を取り付け、プラグインが子ウィンドウを作るので、その前に始める
        EditorGuard::protect (*this, std::move (onEditorFault));
        setVisible (true);
        toFront (true); // JUCE の setVisible はアクティブにしない（SW_SHOWNA）ので、ここで手前に出してアクティブにする（1 回だけ）

        // JUCE 8 既定の Direct2D 描画だと、枠のドラッグでサイズを変えたときに中身がずれることがある。
        // トップレベルウィンドウごとに切り替えが必要なので、プラグインの画面でも切り替える
        if (auto* peer = getPeer())
            peer->setCurrentRenderingEngine (0);

        startTimer (1500); // プラグインが描き終わるのを待ってから最初に撮る
    }

    // 閉じる直前の画面も撮る（閉じるボタン以外で閉じたとき: プリセットの読み込み、アプリの終了など）
    ~PluginWindow() override
    {
        stopTimer();

        // 例外が起きた画面は、消す間もプラグインにメッセージを渡さない（包むのをやめるのは、消したあとで呼び出し元が行う）
        if (EditorGuard::hasFaulted (*this))
            return;

        takeSnapshot();
        EditorGuard::release (this);
    }

    // もう撮らない（エラーで止まったプラグインの画面は、描かせると処理側と同じロックを待って固まることがあるので、閉じる前に呼ぶ）
    void disableSnapshots()
    {
        stopTimer();
        onSnapshot = nullptr;
    }

    void closeButtonPressed() override
    {
        takeSnapshot();
        if (onClose != nullptr)
            onClose();
    }

    void moved() override
    {
        DocumentWindow::moved();
        notifyBounds();
    }

    void resized() override
    {
        DocumentWindow::resized();
        notifyBounds();
    }

private:
    void timerCallback() override
    {
        takeSnapshot();
        startTimer (5000);
    }

    void takeSnapshot()
    {
        if (onSnapshot == nullptr || EditorGuard::hasFaulted (*this))
            return;
        if (const auto image = WindowCapture::captureClientArea (*this); image.isValid())
            onSnapshot (image);
    }

    void notifyBounds()
    {
        if (onBoundsChanged != nullptr && isVisible() && ! isMinimised())
            onBoundsChanged (getBounds());
    }

    // タイトルバーのあたりがどこかの画面の作業領域に入っていれば、つかんで動かせるので画面の中とみなす
    static bool isOnScreen (juce::Rectangle<int> bounds)
    {
        const auto titleArea = bounds.withHeight (40).reduced (40, 0);
        for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
            if (display.userBounds.toNearestInt().intersects (titleArea))
                return true;
        return false;
    }

    std::function<void (juce::Rectangle<int>)> onBoundsChanged;
    std::function<void()> onClose;
    std::function<void (const juce::Image&)> onSnapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};
