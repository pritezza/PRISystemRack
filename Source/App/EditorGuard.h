#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

struct _EXCEPTION_POINTERS; // <windows.h> の EXCEPTION_POINTERS（ここでは windows.h を読み込まない）

// プラグインの画面の中で起きた例外（アクセス違反など）でアプリごと落ちないようにする。
// プラグインの画面の窓（PluginWindow）の下にある子ウィンドウ（プラグインが作るもの、JUCE が埋め込みに使うもの）の
// メッセージ処理を、例外を受け止める処理で包む。あとから作られる子ウィンドウも、作られたときに包む（CBT フック）。
// 例外を受け止めたら、その画面の子ウィンドウにはもうメッセージを渡さず（プラグインの中の状態が壊れているため）、
// onFault を後で 1 回呼ぶ（呼び出し元が画面を閉じる）。メッセージスレッドで作られた窓だけが対象
// （プラグインが自分のスレッドで作った窓の中で落ちたときは防げない）。メッセージスレッドだけで使う
namespace EditorGuard
{
void protect (juce::Component& pluginWindow, std::function<void()> onFault);
// 包むのをやめる。普段は窓を消す前に呼ぶ。例外が起きた窓は、消す間も包んだまま（プラグインに渡さない）にして、
// 消したあとで呼ぶ（アドレスで探すだけなので、消したあとのポインターでよい）
void release (const juce::Component* pluginWindow);
bool hasFaulted (const juce::Component& pluginWindow);

// アプリの起動時に（メッセージスレッドで）1 回呼ぶ。これ以後メッセージスレッドで作られる窓のうち、窓の処理がプラグインの
// DLL の中にあるもの（本体と Windows の DLL 以外）も、PluginWindow の外にあっても包む（プラグインが自分で作った窓。
// DodgePro はそういう窓の処理の中で 0 で割って落ちた）。例外が起きたら、その DLL の窓すべてにメッセージを渡さなくなり、知らせを出す
void installForPlugins();

// fn を例外を受け止めて呼ぶ（例外を起こしたプラグインの画面を消すときなど）。例外が起きたら false
bool callGuarded (const std::function<void()>& fn);

// 誰も受け止めなかった例外（アプリの例外フィルターに届いたもの）から戻せるか試す。どのスレッドからでも呼べる。
// プラグインが窓の処理を自分の関数に差し替えると、その関数は上の包みの外側に来るので、そこで起きた例外は包みに届かない。
// 落ちた所から呼び出し元をたどり、プラグインの関数だけを飛ばして（手前に許すのは、プラグインから呼ばれてロックを持たない
// Windows の処理だけ）プラグインの窓の処理を Windows（user32）が呼んだ所まで戻れるなら、窓の処理が 0 を返したことにして
// info の続き先をそこに書き換え、true を返す（呼び出し元は EXCEPTION_CONTINUE_EXECUTION を返す）。
// 後でメッセージスレッドで、その DLL の窓を止め、画面を閉じて知らせる。止められる窓がなかった DLL と、
// 短い時間に何度も戻した DLL では、もう戻さない（落ちるに任せ、クラッシュの記録を残す）。installForPlugins のあとで使う
bool recoverUnhandled (::_EXCEPTION_POINTERS* info);
} // namespace EditorGuard
