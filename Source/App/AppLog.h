#pragma once

#include <juce_core/juce_core.h>

// 本体アプリのログ: %ProgramData%\PRISystemRack\Logs\app.log に 1 行ずつ追記する（APO・Setup のログと同じフォルダー）。
// 配布先で「音が出ない」「止まった」と言われたときの手がかりにする。書くのは状態が変わったときだけ（周期ごとには書かない）。
//
// - 1 MB を超えたら app.old.log に回して新しく書き始める（ファイルは最大 2 つ。前の app.old.log は消える）
// - どのスレッドから呼んでもよい（1 行ごとに開いて閉じる）。ただし音声スレッドからは呼ばないこと
// - ほかの Windows ユーザーが作った app.log には書けない（ProgramData の下のファイルは作った人だけが書ける）ので、そのときは
//   app-<ユーザー名>.log に書く
// - 書けなければ（フォルダーに書く権限がないなど）何もしない
namespace AppLog
{
void write (const juce::String& message);
juce::File file(); // 今書いているファイル（まだ書いていなければ app.log）
} // namespace AppLog
