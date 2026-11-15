#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>

// 入力・出力の欄のメーターに渡す値（前回読んでからの分）。[0] = 左、[1] = 右
struct LevelReading
{
    float peak[2] {};          // サンプルのピーク（絶対値の最大）
    double sumSquares[2] {};   // 2 乗の和（RMS・VU・K-System 用。画面側が時間窓で平均する）
    double samples[2] {};      // sumSquares に足したサンプルの数
    double loudnessSum = 0.0;  // K 特性（ITU-R BS.1770）をかけた 2 乗の、チャンネルの重み付きの和
    double loudnessFrames = 0.0;
};

// 入力・出力のメーターの値を、オーディオのスレッドで集める。
// オーディオのスレッドはブロックごとに beginBlock → add（サンプルごと）→ endBlock を呼び、画面は take で読んで 0 に戻す。
//
// - 左右: 1 つ目のチャンネルを左、2 つ目を右にする。1 チャンネルなら左右とも、3 つ目以降のチャンネルは左右の両方に入れる
// - ラウドネス: BS.1770 の K 特性（高域のシェルフ + 低域のハイパス）をかけた 2 乗の和。
//               6 チャンネル以上（5.1 の並び）では 4 つ目（LFE）を除き、5・6 つ目（サラウンド）を 1.41 倍にする
// - NaN・無限大は 0 として数える（プラグインが壊れた値を出しても、メーターが振り切れたまま・-inf のまま固まらないように。
//   出力の保護（OutputGuard）が値を直すのはメーターのあと）
class LevelMeterTap
{
public:
    static constexpr uint32_t maxFilteredChannels = 8; // K 特性の状態を持つチャンネルの数（それより後ろはピークと RMS だけ）

    void beginBlock (uint32_t sampleRate, uint32_t numChannels)
    {
        if (sampleRate != preparedRate)
            prepare (sampleRate);
        channels = numChannels;
        block = {};
    }

    void add (uint32_t ch, float x)
    {
        if (! std::isfinite (x))
            x = 0.0f;
        const float level = std::abs (x);

        if (ch < maxFilteredChannels)
        {
            auto& s = state[ch];

            // K 特性（2 つの biquad。転置直接形 II）
            const double in = (double) x;
            const double shelf = shelfB[0] * in + s.z[0];
            s.z[0] = shelfB[1] * in - shelfA[0] * shelf + s.z[1];
            s.z[1] = shelfB[2] * in - shelfA[1] * shelf;
            const double high = shelf + s.z[2]; // ハイパスの分子は 1, -2, 1
            s.z[2] = -2.0 * shelf - highA[0] * high + s.z[3];
            s.z[3] = shelf - highA[1] * high;

            if (const double weight = loudnessWeight (ch); weight > 0.0)
                block.loudnessSum += weight * high * high;
        }

        if (ch == 0)
            block.loudnessFrames += 1.0;

        const bool both = channels == 1 || ch >= 2;
        if (both || ch == 0)
            addToSide (0, level);
        if (both || ch == 1)
            addToSide (1, level);
    }

    void endBlock()
    {
        // 無音が続いたときに非正規化数で遅くならないよう、ごく小さな値は 0 にする
        for (auto& s : state)
            for (auto& z : s.z)
                if (std::abs (z) < 1.0e-20)
                    z = 0.0;

        raise (iconPeak, juce::jmax (block.peak[0], block.peak[1]));
        iconSumSquares.fetch_add (block.sumSquares[0] + block.sumSquares[1], std::memory_order_relaxed);
        iconSamples.fetch_add (block.samples[0] + block.samples[1], std::memory_order_relaxed);
        for (int side = 0; side < 2; ++side)
        {
            raise (shared.peak[side], block.peak[side]);
            shared.sumSquares[side].fetch_add (block.sumSquares[side], std::memory_order_relaxed);
            shared.samples[side].fetch_add (block.samples[side], std::memory_order_relaxed);
        }
        shared.loudnessSum.fetch_add (block.loudnessSum, std::memory_order_relaxed);
        shared.loudnessFrames.fetch_add (block.loudnessFrames, std::memory_order_relaxed);
    }

    // 画面のスレッド: 前回読んでからの値を読んで 0 に戻す
    LevelReading take()
    {
        LevelReading r;
        for (int side = 0; side < 2; ++side)
        {
            r.peak[side] = shared.peak[side].exchange (0.0f);
            r.sumSquares[side] = shared.sumSquares[side].exchange (0.0);
            r.samples[side] = shared.samples[side].exchange (0.0);
        }
        r.loudnessSum = shared.loudnessSum.exchange (0.0);
        r.loudnessFrames = shared.loudnessFrames.exchange (0.0);
        return r;
    }

    // 画面とは別の読み手（通知領域のアイコン）用: 前回読んでからの、左右の大きい方のピークと、左右を合わせた RMS を読んで 0 に戻す
    // （音が来ていなければ rms は 0）。take とは別にためるので、画面のメーターの値を横取りしない
    struct IconLevel
    {
        float peak = 0.0f;
        float rms = 0.0f;
    };
    IconLevel takeIconLevel()
    {
        IconLevel level;
        level.peak = iconPeak.exchange (0.0f);
        const double sum = iconSumSquares.exchange (0.0), count = iconSamples.exchange (0.0);
        level.rms = count > 0.0 ? (float) std::sqrt (sum / count) : 0.0f;
        return level;
    }

private:
    struct ChannelState
    {
        std::array<double, 4> z {}; // K 特性のフィルターの状態（0, 1: シェルフ、2, 3: ハイパス）
    };

    struct Shared
    {
        std::atomic<float> peak[2] {};
        std::atomic<double> sumSquares[2] {}, samples[2] {};
        std::atomic<double> loudnessSum { 0.0 }, loudnessFrames { 0.0 };
    };

    void addToSide (int side, float level)
    {
        block.peak[side] = juce::jmax (block.peak[side], level);
        block.sumSquares[side] += (double) level * (double) level;
        block.samples[side] += 1.0;
    }

    double loudnessWeight (uint32_t ch) const
    {
        if (channels < 6)
            return 1.0;
        if (ch == 3)
            return 0.0; // LFE
        return ch == 4 || ch == 5 ? 1.41 : 1.0;
    }

    static void raise (std::atomic<float>& target, float value)
    {
        if (value > target.load (std::memory_order_relaxed))
            target.store (value, std::memory_order_relaxed);
    }

    void prepare (uint32_t sampleRate)
    {
        preparedRate = sampleRate;
        for (auto& s : state)
            s = {};

        if (sampleRate < 8000) // 準備ができる前（0）など: 素通しにする
        {
            shelfB = { 1.0, 0.0, 0.0 };
            shelfA = highA = { 0.0, 0.0 };
            return;
        }
        const double fs = (double) sampleRate;

        // K 特性（ITU-R BS.1770。任意のサンプルレート向けの式）
        {
            const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
            const double k = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double vh = std::pow (10.0, gain / 20.0);
            const double vb = std::pow (vh, 0.4996667741545416);
            const double a0 = 1.0 + k / q + k * k;
            shelfB = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0 };
            shelfA = { 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double k = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double a0 = 1.0 + k / q + k * k;
            highA = { 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
        }
    }

    uint32_t preparedRate = 0, channels = 2;
    std::array<double, 3> shelfB {};
    std::array<double, 2> shelfA {}, highA {};
    std::array<ChannelState, maxFilteredChannels> state {};
    LevelReading block; // このブロックの分（オーディオのスレッドだけ）
    Shared shared;      // 画面が読むまでためておく分
    std::atomic<float> iconPeak { 0.0f }; // 通知領域のアイコンが読むまでためておくピーク
    std::atomic<double> iconSumSquares { 0.0 }, iconSamples { 0.0 }; // 同じく RMS（左右を合わせた 2 乗の和とその数）
};
