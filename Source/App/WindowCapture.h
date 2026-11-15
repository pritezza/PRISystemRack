#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace WindowCapture
{
// トップレベルのウィンドウの中身（タイトルバーと枠を除く）を撮る。プラグインの画面は、プラグインが自分で描く Windows の
// ウィンドウなので JUCE の createComponentSnapshot では写らない。Windows の PrintWindow（PW_RENDERFULLCONTENT。
// GPU で描く画面も写る）で撮る。最小化・非表示のときや、撮れなかったとき（真っ黒など）は無効な Image を返す
juce::Image captureClientArea (juce::Component& topLevelWindow);
} // namespace WindowCapture
