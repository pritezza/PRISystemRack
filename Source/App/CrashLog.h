#pragma once

#include <juce_core/juce_core.h>

// アプリが落ちたときに、落ちた場所の呼び出し経路（関数名・ファイル・行。exe と同じフォルダーの PDB を使う）と
// ミニダンプを %APPDATA%\PRISystemRack\Crashes に書く。起動の最初に 1 回呼ぶ
namespace CrashLog
{
void install();
juce::File folder();

// プラグインが自分の処理に差し替えることがあるので、ときどき（タイマーから）登録し直す
void reinstall();
} // namespace CrashLog
