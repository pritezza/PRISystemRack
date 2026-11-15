#pragma once

#include <juce_core/juce_core.h>

// 画面に出す文字列は英語で書いて TRANS("...") で囲む（日本語は Resources/Translations/ja.txt）。
// TRANS に渡す文字列は ASCII だけにすること（juce::String が const char* を ASCII として扱うため）。

// 画面に出すアプリの名前（メイン画面のタイトルバー。実行ファイルや設定のフォルダーの名前は PRISystemRack のまま）
inline const char* const appDisplayName = "PRI System Rack";

// UTF-8 の文字列リテラルを juce::String にする（訳さない文字列、例: 言語名「日本語」）
inline juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}
