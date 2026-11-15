#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// 「常に手前に表示」（表示メニュー。設定の alwaysOnTop）。メッセージスレッドだけで読み書きする。
// メイン画面だけ手前にすると、プラグインの画面やアプリ用ラックのウィンドウが重なったときにその下に隠れるので、
// アプリのウィンドウすべてに同じ設定をかける（確認の画面やダイアログは、JUCE が手前のウィンドウがあれば自分も手前にする）
namespace AlwaysOnTop
{
inline bool enabled = false;

// 新しく作るウィンドウに今の設定をかける（表示する前に呼ぶ）
inline void applyTo (juce::Component& window)
{
    window.setAlwaysOnTop (enabled);
}

// 設定を変え、開いているウィンドウすべてにかける
inline void setEnabled (bool on)
{
    enabled = on;
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* window = juce::TopLevelWindow::getTopLevelWindow (i))
            window->setAlwaysOnTop (on);
}
} // namespace AlwaysOnTop
