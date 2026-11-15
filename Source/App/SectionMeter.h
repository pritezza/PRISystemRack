#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <deque>

#include "LevelMeterTap.h"

// 入力・出力の欄のメーター（画面側）。LevelMeterTap から読んだ値を種類ごとの表示に直して描く。
// どの種類の値も毎回計算しておくので、種類を切り替えるとすぐにその表示になる（インテグレーテッドは画面を開いてからの分）。
//
// - ピーク: サンプルのピーク。落ちるのは 40 dB/秒
// - RMS: 300 ms の平均（サイン波はピークより 3 dB 低く読む）
// - ピーク＋RMS: RMS のバーの後ろにピークを薄く重ねる
// - VU: 300 ms の平均。サイン波で読むよう 3 dB 足し（AES17）、-18 dBFS を 0 VU にする
// - ラウドネス（ITU-R BS.1770 / EBU R128）: モーメンタリー 400 ms、ショートターム 3 秒、インテグレーテッド（-70 LUFS と -10 LU のゲート）。
//   左右を合わせた 1 つの値なので、バーは 1 本
// - K-System: RMS（AES17）の 0 を -20 / -14 / -12 dBFS に置いた目盛り。0 から黄色、+4 から赤。ピークを薄く重ねる
class SectionMeter
{
public:
    enum class Kind
    {
        peak, rms, peakRms, vu,
        lufsMomentary, lufsShortTerm, lufsIntegrated, k20, k14, k12,
        none // 表示しない（場所は残し、ホイールや右クリックで戻せる）
    };

    static const std::vector<Kind>& order(); // ホイールで切り替える順（メニューの並びも同じ）
    static Kind next (Kind kind, int steps);
    static bool startsGroup (Kind kind);         // メニューでこの種類の前に区切りを入れるか（ラウドネス、K-System、表示しない）
    static juce::String name (Kind kind);        // メニューに出す名前
    static juce::String toId (Kind kind);        // 設定に保存する名前
    static Kind fromId (const juce::String& id); // 知らない名前（なくした PPM の ppmEbu / ppmBbc / ppmDin も）ならピーク＋RMS（既定）

    // 画面のタイマーから呼ぶ（メーターの fps 回/秒）
    void update (const LevelReading& reading, Kind kind);
    void resetIntegrated();

    // meter: バーの枠（空なら出さない）。readout: 右の数値の欄
    void paint (juce::Graphics& g, Kind kind, juce::Rectangle<float> meter, juce::Rectangle<int> readout,
                bool peakHold) const;

private:
    struct Entry // タイマー 1 回分の値（RMS とラウドネスの時間窓に使う）
    {
        juce::uint32 timeMs = 0;
        double sumSquares[2] {}, samples[2] {};
        double loudnessSum = 0.0, loudnessFrames = 0.0;
    };

    float mainValue (Kind kind, int side) const;
    float holdSource (Kind kind, int side) const;
    void windowSums (juce::uint32 now, juce::uint32 windowMs, Entry& sum) const;
    float integratedLoudness() const;

    static constexpr float floorDb = -100.0f;
    static constexpr int histogramBins = 750; // インテグレーテッド: -70〜+5 LUFS を 0.1 LU 刻みで数える

    juce::uint32 lastUpdateMs = 0;
    float peakDb[2] { floorDb, floorDb };
    float rmsDb[2] { floorDb, floorDb };
    float momentary = floorDb, shortTerm = floorDb, integrated = floorDb;

    std::deque<Entry> history; // 直近 3 秒あまり
    std::array<juce::uint32, histogramBins> histogram {};
    juce::uint32 lastGateBlockMs = 0; // インテグレーテッド: 100 ms ごとに 400 ms の区間を数える

    Kind holdKind = Kind::peak;
    float holdDb[2] { floorDb, floorDb };
    juce::uint32 holdSinceMs[2] {};
};
