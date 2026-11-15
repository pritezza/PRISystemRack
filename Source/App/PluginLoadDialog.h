#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// プラグインを足す・置き換えるときの、進み具合のダイアログ。
//
// 1. プラグイン安全チェック（ON のとき。PluginSafety）: 補助プログラムで試す段階を順に並べ、済んだもの・今のもの・失敗したものに
//    印を付け、増えた GDI / USER オブジェクトの数と経過時間を出す。補助プログラムの終わりは待たずにタイマーで見るので、
//    画面は止まらない。［キャンセル］で補助プログラムを止めて取りやめる（本体には何も作っていないので安全）。
// 2. 本体に読み込む: VST3 のプラグインは画面と同じスレッドで作る決まりがあり、その間は画面が止まるので、
//    「本体に読み込んでいます（キャンセルできません）」と出してから読み込む（ダイアログを描いてから始める）。
//
// 起動している間に試して読み込めないと分かっているプラグインは、理由を出して「もう一度チェックしますか？」と聞く。
namespace PluginLoadDialog
{
// load: 本体に読み込む処理（メッセージスレッドで呼ぶ。失敗なら false と error）。
// done: 終わったら 1 回だけ呼ぶ（ダイアログを閉じたあと）。loaded = 読み込めた。cancelled = 利用者が取りやめた（error は空）。
// どちらでもなければ、error に読み込めなかった理由（読み込めないと分かっていて、理由を出して「チェックし直さない」を選んだときは空）
void run (const juce::PluginDescription& description,
          std::function<bool (juce::String& error)> load,
          std::function<void (bool loaded, bool cancelled, const juce::String& error)> done);
} // namespace PluginLoadDialog
