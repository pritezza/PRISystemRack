#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// マウスホイールの量を目盛りの数にする。通常のマウスは 1 目盛りで deltaY が約 0.23
// （JUCE は Windows の 1 目盛り 120 を半分にして 256 で割る）。タッチパッドなどの細かい量はためてから数える
class WheelSteps
{
public:
    // 縦のホイールの量（isReversed を考えた向き。上へ回すと正）
    static float vertical (const juce::MouseWheelDetails& wheel)
    {
        return wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    }

    // 横のホイールがあればそれを（右へ回すと負）、なければ縦を使う
    static float verticalOrHorizontal (const juce::MouseWheelDetails& wheel)
    {
        const float delta = wheel.deltaX != 0.0f ? -wheel.deltaX : wheel.deltaY;
        return wheel.isReversed ? -delta : delta;
    }

    // delta をためて、目盛りに達したらその数を返す（少し足りなくても 1 目盛りとみなす）。達しなければ 0
    int take (float delta)
    {
        constexpr float notch = 0.5f * 120.0f / 256.0f;
        pending += delta;
        const int steps = (int) (pending / (notch * 0.9f));
        if (steps != 0)
            pending = 0.0f;
        return steps;
    }

private:
    float pending = 0.0f;
};
