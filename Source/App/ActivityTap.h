#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <vector>

// プラグインに渡す前の音と、処理した後の音（どちらも左右。モノラルなら左右に同じ音、3 チャンネル以上なら先頭の 2 つ）を、
// 行のメーター（増減、スペクトル、位相相関、ゴニオメーターなど）のために貯めておくリングバッファ。
// 書くのは音声スレッド（PluginChain::process）だけ、読むのはメッセージスレッドだけ。
// 読む側は、書いている位置から十分離れた所だけを読む（書き込みと重ならないように）
class ActivityTap
{
public:
    static constexpr int size = 1 << 16; // 65536 サンプル（48 kHz で約 1.4 秒）。レイテンシーの補正もこの中で行う
    static constexpr int writeMargin = 8192; // 読んでいる間に書き進められても重ならないよう空けておく分

    ActivityTap() : inL ((size_t) size), inR ((size_t) size), outL ((size_t) size), outR ((size_t) size) {}

    // 音声スレッド。in はプラグインに渡す前、out は処理した後（どちらも channels 本、frames 個）
    void push (const float* const* in, const float* const* out, int channels, int frames, juce::uint32 sampleRate) noexcept
    {
        if (channels <= 0 || frames <= 0)
            return;

        const auto start = written.load (std::memory_order_relaxed);
        const int right = channels > 1 ? 1 : 0;

        for (int i = 0; i < frames; ++i)
        {
            const auto index = (size_t) ((start + (juce::uint64) i) & mask);
            inL[index] = in[0][i];
            inR[index] = in[right][i];
            outL[index] = out[0][i];
            outR[index] = out[right][i];
        }

        rate.store (sampleRate, std::memory_order_relaxed);
        lastPushMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
        written.store (start + (juce::uint64) frames, std::memory_order_release);
    }

    // メッセージスレッド。直近の count サンプルの出力（左右）と、それと時間を合わせた入力（latency サンプル前。左右）を取り出す。
    // まだ足りなければ false
    bool read (float* inLeft, float* inRight, float* outLeft, float* outRight, int count, int latency) const
    {
        const auto end = written.load (std::memory_order_acquire);
        latency = juce::jlimit (0, juce::jmax (0, size - count - writeMargin), latency);

        if (end < (juce::uint64) (count + latency))
            return false;

        for (int i = 0; i < count; ++i)
        {
            const auto outPos = end - (juce::uint64) count + (juce::uint64) i;
            const auto outIndex = (size_t) (outPos & mask);
            const auto inIndex = (size_t) ((outPos - (juce::uint64) latency) & mask);
            outLeft[i] = outL[outIndex];
            outRight[i] = outR[outIndex];
            inLeft[i] = inL[inIndex];
            inRight[i] = inR[inIndex];
        }
        return true;
    }

    // 最近（0.3 秒以内）に処理したか。OFF・休止中・止まっているときは false
    bool isFresh() const
    {
        return written.load (std::memory_order_relaxed) > 0
            && juce::Time::getMillisecondCounter() - lastPushMs.load (std::memory_order_relaxed) < 300;
    }

    juce::uint32 getSampleRate() const { return rate.load (std::memory_order_relaxed); }

    //==============================================================================
    // 処理の重さ: 1 周期の processBlock にかかった時間 ÷ 周期の長さ（1.0 = 周期いっぱい）。
    // recordLoad は音声スレッド、ほかはメッセージスレッド
    void recordLoad (float load) noexcept
    {
        loadAverage.store (loadAverage.load (std::memory_order_relaxed) * 0.95f + load * 0.05f, std::memory_order_relaxed);
        if (load > loadPeak.load (std::memory_order_relaxed))
            loadPeak.store (load, std::memory_order_relaxed);
    }

    float getLoadAverage() const { return loadAverage.load (std::memory_order_relaxed); }
    // 前に読んでからの最大（読むと 0 に戻す。読むのは表示 1 つだけ）
    float takeLoadPeak() const { return loadPeak.exchange (0.0f, std::memory_order_relaxed); }

private:
    static constexpr juce::uint64 mask = (juce::uint64) (size - 1);

    std::vector<float> inL, inR, outL, outR;
    std::atomic<juce::uint64> written { 0 };
    std::atomic<juce::uint32> rate { 48000 };
    std::atomic<juce::uint32> lastPushMs { 0 };
    std::atomic<float> loadAverage { 0.0f };
    mutable std::atomic<float> loadPeak { 0.0f };
};
