#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginChain.h"

// プラグインの行の、名前の右に出す小さなメーター。プラグインごとに選ぶ（PluginChain::Plugin::view）:
// - 増減バー: 出力と入力のレベルの差（中央が 0 dB。左へ伸びれば削っている、右へ伸びれば足している）と数値
// - 入力と出力のレベル: 横向きの 2 本のバー（上が入力、下が出力）と差の数値
// - 増減の履歴: 増減バーの値の直近 4 秒を、右から左へ流れる線で
// - 周波数ごとの増減: 入力と出力を周波数ごとに分けて（FFT）、どの帯域をどれだけ削っているか・足しているか
// - 出力のスペクトル: 出力の周波数ごとの強さ（塗り）と、入力（薄い線）
// - 処理の重さ: 1 周期の processBlock にかかった時間 ÷ 周期の長さ（平均と、最近の最大）
// - 位相相関: 出力の左右の相関（-1〜+1。+1 は左右が同じ、0 は無関係、マイナスは逆相）。入力の値を薄い印で
// - ゴニオメーター: 出力の左右を、縦が中央（L+R）、横が左右の差（L-R）の点で描く。右に位相相関の数値
// 増減は、入力をプラグインのレイテンシー分遅らせて出力と時間を合わせてから比べる。
// マウスの操作は受けない（行が受けて、クリックでエディターを開き、ホイールで表示を切り替える）
class PluginActivityView final : public juce::Component
{
public:
    PluginActivityView();

    // タイマーから呼ぶ（1 秒に getFramesPerSecond() 回）。plugin がなければ空
    void update (const PluginChain::Plugin* plugin);

    // メーターの更新の回数（1 秒あたり。表示メニューの「メーターの fps」。設定の meterFps。既定 20）。
    // なめらかにする量・下がる速さ・履歴の長さは、回数によらず時間でそろえる
    static constexpr int fpsChoices[] = { 5, 10, 15, 20, 25, 30 };
    static constexpr int defaultFramesPerSecond = 20;
    static void setFramesPerSecond (int fps);
    static int getFramesPerSecond() { return framesPerSecond; }

    void paint (juce::Graphics&) override;

    // 表示の名前（メニュー用）
    static juce::String viewName (PluginChain::Plugin::View view);

    // 表示の順（メニューとホイール。最後が「表示しない」。そのときも場所は残すので、ホイールで戻せる）
    static const std::vector<PluginChain::Plugin::View>& viewOrder();

    // ホイールで steps だけ進めた表示
    static PluginChain::Plugin::View nextView (PluginChain::Plugin::View view, int steps);

private:
    using View = PluginChain::Plugin::View;

    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder; // 2048 サンプル（48 kHz で 43 ms。周波数の細かさは 23 Hz）
    static constexpr int bandCount = 28;           // 周波数ごとの増減の帯域の数（40 Hz〜18 kHz を対数で等分）
    static constexpr int analyserBandCount = 48;   // 出力のスペクトルの帯域の数
    static constexpr float analyserFloorDb = -90.0f; // 出力のスペクトルの下端（上端は 0 dBFS）
    static constexpr int historySeconds = 4;       // 増減の履歴の長さ（点の数は fps × 4）
    inline static int framesPerSecond = defaultFramesPerSecond;
    static int historyLength() { return framesPerSecond * historySeconds; }
    static constexpr float rangeDb = 12.0f;        // 増減の表示の範囲（±12 dB）
    static constexpr int goniometerPoints = 512;   // ゴニオメーターに描く点の数

    void analyseLevels (const float* in, const float* out);
    void analyseSpectrum (const float* in, const float* out, double sampleRate);
    void analyseOutputSpectrum (const float* in, const float* out, double sampleRate);
    void analyseStereo();
    void bandPowers (const float* source, int bands, float* powers, double sampleRate); // FFT して帯域ごとの強さ（平均）
    void resetDisplay();

    void paintDeltaBar (juce::Graphics&, juce::Rectangle<float> area);
    void paintLevelBars (juce::Graphics&, juce::Rectangle<float> area);
    void paintHistory (juce::Graphics&, juce::Rectangle<float> area);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float> area);
    void paintOutputSpectrum (juce::Graphics&, juce::Rectangle<float> area);
    void paintLoad (juce::Graphics&, juce::Rectangle<float> area);
    void paintCorrelation (juce::Graphics&, juce::Rectangle<float> area);
    void paintGoniometer (juce::Graphics&, juce::Rectangle<float> area);
    static juce::String formatDelta (float db);
    static juce::String formatCorrelation (float value);

    View view = View::none;
    bool fresh = false; // 最近処理した（OFF・休止中・止まっているときは false）

    // 取り出した音（左右と、左右の平均）
    std::vector<float> inL, inR, outL, outR, inMono, outMono, fftData, window;
    juce::dsp::FFT fft { fftOrder };

    std::array<float, analyserBandCount> analyserIn {}, analyserOut {}; // 出力のスペクトル（dB。落ちるときはゆっくり）
    float loadAverage = 0.0f, loadPeak = 0.0f;  // 処理の重さ（1.0 = 周期いっぱい）。最大はゆっくり下げる
    float correlationOut = 0.0f, correlationIn = 0.0f; // 位相相関（なめらかにしたもの）
    bool correlationValid = false;
    std::vector<juce::Point<float>> goniometer; // ゴニオメーターの点（-1〜1 に収めたもの）

    float inDb = -100.0f, outDb = -100.0f; // 入力・出力のレベル（表示用。落ちるときはゆっくり）
    float deltaDb = 0.0f;                  // 増減（なめらかにしたもの）
    bool deltaValid = false;               // 入力か出力に音がある
    std::vector<float> history;            // 増減の履歴（音がないところは NaN）
    std::array<float, bandCount> bandIn {}, bandOut {}; // 帯域ごとの強さ（なめらかにしたもの）
    std::array<float, bandCount> bandDelta {};
    std::array<bool, bandCount> bandValid {};
};

// プラグインの行の、メーターの右に常に出すエディターの画面の縮小（エディターを開いている間と閉じる直前に撮った静止画）。
// まだ撮っていなければ案内を出す。マウスの操作は受けない（行が受けて、クリックでエディターを開く）
class PluginSnapshotView final : public juce::Component
{
public:
    PluginSnapshotView() { setInterceptsMouseClicks (false, false); }

    // タイマーから呼ぶ。撮り直していたら描き直す
    void update (const PluginChain::Plugin* plugin);

    void paint (juce::Graphics&) override;

private:
    juce::Image snapshot;
    int snapshotVersion = -1;
};
