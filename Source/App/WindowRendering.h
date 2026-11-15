#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// トップレベルのウィンドウの描画方式。
// 普段は Direct2D で描く（文字を DirectWrite で描くので、ソフトウェア描画よりくっきりする）。
// JUCE 8 の Direct2D には、ネイティブの枠をマウスでドラッグしてサイズを変えると中身がタイトルバーの高さ分ずれる不具合があるので、
// そのドラッグの間だけソフトウェア描画に切り替え、ドラッグが終わったら Direct2D に戻す。
// ディスプレイの構成が変わった後（抜き差しなど）も同じずれが残るので、その後しばらくはウィンドウの動きが止まるたびに描き直させる
namespace WindowRendering
{
// ウィンドウを表示した後（ピアができた後）に呼ぶ。同じウィンドウに何度呼んでもよい
void install (juce::Component& topLevelWindow);
} // namespace WindowRendering
