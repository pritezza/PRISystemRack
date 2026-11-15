#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>

// 音声テスト用のサイン波（440 Hz、-20 dBFS、すべてのチャンネルに同じ音）。
// ON/OFF はメッセージスレッドで切り替え、音声スレッドが周期ごとに足す。
// 鳴り始めと止めるときは 20 ms かけて音量を変える（ぷつっと鳴らないように）
class TestTone
{
public:
    static constexpr double frequency = 440.0;
    static constexpr float amplitude = 0.1f; // -20 dBFS

    void setOn (bool shouldBeOn) { on = shouldBeOn; }
    bool isOn() const { return on.load(); }

    // 音声スレッド: data にサイン波を足す
    void addTo (float* const* data, uint32_t channels, uint32_t frames, uint32_t sampleRate)
    {
        const float target = on.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
        if (sampleRate == 0 || (target == 0.0f && fade == 0.0f))
        {
            phase = 0.0;
            return;
        }

        const float fadeStep = 1.0f / (0.02f * (float) sampleRate);
        const double increment = juce::MathConstants<double>::twoPi * frequency / (double) sampleRate;

        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            fade = target > fade ? juce::jmin (target, fade + fadeStep) : juce::jmax (target, fade - fadeStep);
            const float sample = (float) std::sin (phase) * amplitude * fade;
            phase += increment;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            for (uint32_t ch = 0; ch < channels; ++ch)
                data[ch][frame] += sample;
        }
    }

private:
    std::atomic<bool> on { false };
    double phase = 0.0; // 音声スレッドだけが使う
    float fade = 0.0f;
};
