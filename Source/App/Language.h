#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// 画面の言語。ソースの文字列は英語で書いて TRANS() で囲み、日本語は Resources/Translations/ja.txt で訳す。
// 訳の中で値を差し込む場所は %name% のように書き、juce::String::replace で置き換える。
namespace Language
{
inline constexpr const char* english = "en";
inline constexpr const char* japanese = "ja";

// "en" / "ja" を反映する（訳とフォント）。知らない値なら英語
void apply (const juce::String& code);
juce::String current();
} // namespace Language
