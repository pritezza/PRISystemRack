#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>

// 出力の保護（安全リミッター）。メインのラックの出力の最後（出力ゲインのあと、APO に返す直前）にかける。
// プラグインが壊れた値や大きすぎる音を出したときに、スピーカーや耳を守るため。
//
// - NaN・無限大の値は 0 にする（ドライバーに渡すと大きな雑音になったり止まったりする）
// - 天井（-0.3 dBFS）を超える分は、全チャンネル共通のゲインで瞬時に下げ、100 ms かけて戻す。
//   先読みをしないので遅れは増えない（超えた瞬間は波形の頭を削ることになるが、0 dBFS を超えて割れるよりよい）
// - 設定の outputGuard で切り替える（既定は ON）。OFF のときは何もしない
class OutputGuard
{
public:
    void setEnabled (bool shouldBeEnabled) { enabled.store (shouldBeEnabled, std::memory_order_relaxed); }
    bool isEnabled() const { return enabled.load (std::memory_order_relaxed); }

    // 作動した回数（天井を超えて下げ始めた回数）と、0 にした不正な値（NaN・無限大）の数。画面のステータスバーに出す
    juce::uint64 getLimitCount() const { return limitCount.load (std::memory_order_relaxed); }
    juce::uint64 getInvalidCount() const { return invalidCount.load (std::memory_order_relaxed); }

    // オーディオのスレッド。interleaved: チャンネルが交互に並んだ出力
    void process (float* interleaved, uint32_t frames, uint32_t channels, uint32_t sampleRate)
    {
        if (! isEnabled() || channels == 0)
        {
            gain = 1.0f;
            return;
        }

        if (sampleRate != preparedRate && sampleRate > 0)
        {
            preparedRate = sampleRate;
            release = 1.0f - std::exp (-1.0f / (0.1f * (float) sampleRate)); // 100 ms
        }

        juce::uint64 invalid = 0, limits = 0;
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            float* sample = interleaved + (size_t) frame * channels;

            float peak = 0.0f;
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                if (! std::isfinite (sample[ch]))
                {
                    sample[ch] = 0.0f;
                    ++invalid;
                }
                peak = juce::jmax (peak, std::abs (sample[ch]));
            }

            const float target = peak > ceiling ? ceiling / peak : 1.0f;
            if (target < gain)
            {
                if (gain >= 1.0f)
                    ++limits; // 下げていなかったところから下げ始めた
                gain = target;
            }
            else
            {
                gain = juce::jmin (target, gain + (1.0f - gain) * release);
                if (gain > 0.9999f)
                    gain = 1.0f;
            }

            if (gain < 1.0f)
                for (uint32_t ch = 0; ch < channels; ++ch)
                    sample[ch] *= gain;
        }

        if (invalid > 0)
            invalidCount.fetch_add (invalid, std::memory_order_relaxed);
        if (limits > 0)
            limitCount.fetch_add (limits, std::memory_order_relaxed);
    }

private:
    static constexpr float ceiling = 0.966f; // -0.3 dBFS

    std::atomic<bool> enabled { true };
    std::atomic<juce::uint64> limitCount { 0 }, invalidCount { 0 };

    // オーディオのスレッドだけ
    float gain = 1.0f;
    float release = 0.0002f;
    uint32_t preparedRate = 0;
};
