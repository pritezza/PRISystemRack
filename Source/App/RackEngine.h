#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "AppRack.h"
#include "HostCore/ApoLink.h"
#include "HostCore/ProcessCapture.h"
#include "PluginChain.h"
#include "OutputGuard.h"
#include "ProcessingBuffer.h"
#include "RackModel.h"
#include "TestTone.h"

// メインのラック。APO から届いた音（Windows の再生音すべて）を処理する。
//
// 処理の順番:
//   入力 → 入力ゲイン → プラグイン（PluginChain）→ ［アプリ用ラックの音を足す］→ 出力ゲイン → 出力
//
// アプリ用ラックも持ち、周期ごとに render させて足す。アプリ用ラックがあるときは、ほかに何も鳴っていなくても
// APO が動くよう、再生デバイスに無音を流し続ける（SilentStream）。
//
// スレッド: 構成の変更はメッセージスレッド。Prepare / Process は音声スレッド（ApoLink のスレッド）で、
// 作業用バッファとアプリ用ラックの並びは processLock で守る。
class RackEngine final : public RackModel,
                         public pri::host::ApoLink::Processor,
                         public juce::ChangeBroadcaster, // 構成が変わったら知らせる（メッセージスレッド）
                         private juce::ChangeListener,
                         private juce::AsyncUpdater,
                         private juce::Timer // プラグインを 1 つずつ読み込む
{
public:
    RackEngine();
    ~RackEngine() override;

    //==============================================================================
    // 接続する再生デバイス（メッセージスレッド）
    void setEndpoint (const juce::String& friendlyName, const juce::String& guid);
    juce::String getEndpointGuid() const { return endpointGuid; }
    juce::String getEndpointName() const { return endpointName; }
    pri::host::ApoLink::Stats getLinkStats() const;
    juce::String describeFormat() const; // 今の処理形式（APO から知らされたもの。まだなら既定値）

    // 処理バッファ（0〜200 ms。手動で 0 ならオフ）。autoAdjust なら音切れを見て自動で増減する（2 ms 以上）。
    // オフとオンが切り替わるときだけ APO とつなぎ直す（一瞬素通しになる）。量の変更はつなぎ直さない
    void setProcessingBuffer (int milliseconds, bool autoAdjust);
    bool isProcessingBufferOn() const { return processingBuffer != nullptr; }
    double getProcessingBufferCurrentMs() const { return processingBuffer != nullptr ? processingBuffer->getCurrentMilliseconds() : 0.0; }
    int getProcessingBufferSamples() const { return processingBuffer != nullptr ? processingBuffer->getCurrentFrames() : 0; }
    juce::uint64 getBufferUnderruns() const { return processingBuffer != nullptr ? processingBuffer->getUnderruns() : 0; }

    // 処理が間に合っているか（メッセージスレッド。画面と通知領域が読む。前に調べてから 0.4 秒たっていなければ前の結果を返す）。
    // 0.5 秒ごとに、バッファ不足か時間切れが増えたかを見て、直近 2 秒のうち 2 回以上なら「間に合っていない」にする
    // （自動のバッファが増えるときの 1〜2 回の途切れでは出さない）。止まってから 3 秒は出したままにする
    struct Overload
    {
        bool active = false;
        bool buffered = false;    // 処理バッファを使っている（間に合わない周期は無音。使っていなければ APO が素通しにする）
        bool bufferAuto = false;
        bool bufferAtMax = false;
        float load = -1.0f;       // 処理の負荷（処理にかかった時間 ÷ 音の長さ。1.0 で実時間いっぱい）。直近 0.5 秒。分からなければ負
        juce::StringArray heavyPlugins; // 重いプラグイン（「名前 70%」。重い順に 3 つまで）

        juce::String describeTitle() const;  // 何が起きているか（1 行）
        juce::String describeDetail() const; // 負荷・重いプラグイン・対処
    };
    const Overload& checkOverload();
    void previewOverload (double seconds) { previewOverloadUntilMs = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0; } // 確認用

    //==============================================================================
    // アプリ用ラック（メッセージスレッド）
    const std::vector<std::unique_ptr<AppRack>>& getAppRacks() const { return appRacks; }
    static constexpr int maxAppRacks = 10; // アプリ用ラックの数の上限（「＋ ラックを追加」で足せる数。取り込みのスレッドが増えすぎないように）
    AppRack* findAppRack (int id) const;
    AppRack* addAppRack(); // 上限に達していれば・機能が無効なら足さずに nullptr
    void removeAppRack (int id);

    // アプリ用ラックの機能（設定メニュー。既定は無効）。無効のあいだは、構成にあるアプリ用ラックを作らずに中身だけ預かり
    // （音の取り込みもプラグインの読み込みもしない）、saveState でそのまま書き戻す（上書き保存してもラックが消えない）。
    // 有効に戻すと、預かった中身からラックを作り、プラグインを 1 つずつ読み込む。ラックがある状態で無効にすると、ラックを止めて中身を預かる
    void setAppRacksEnabled (bool shouldBeEnabled);
    bool areAppRacksEnabled() const { return appRacksEnabled; }

    //==============================================================================
    // 状態の保存と復元（メッセージスレッド）。
    // 復元では、行（プラグインの並び）とアプリ用ラックをすぐに戻し、プラグインはそのあと 1 つずつ読み込む（1 つ読み込むたびに画面を描き直せる）。
    // 読み込みが終わるまでは、すべての並びを一時停止して音は素通しにし、全部そろったら一度に切り替える。
    // 終わったら onPluginsLoaded を呼ぶ（読み込めなかったプラグインの理由は takeLoadErrors で読む）
    juce::ValueTree saveState() const;
    void restoreState (const juce::ValueTree& state);

    bool isLoadingPlugins() const { return loadingPlugins; }
    int getLoadedCount() const { return loadedCount; } // 読み込みの進み具合（表示用）
    int getLoadTotal() const { return loadTotal; }
    // 次に読み込むプラグインの名前（読み込み中の表示用。メインのラック → アプリ用ラックの順で、まだ読み込んでいない最初のもの）
    juce::String getNextLoadingName();
    juce::StringArray takeLoadErrors() { return std::exchange (loadErrors, {}); }
    std::function<void()> onPluginsLoaded; // 読み込みが終わったとき（メッセージスレッド。画面が受け取る）

    //==============================================================================
    // RackModel
    PluginChain& getChain() override { return chain; }
    juce::ChangeBroadcaster& getModelBroadcaster() override { return *this; }
    float getInputGainDb() const override { return inputGainDb.load(); }
    void setInputGainDb (float db) override { inputGainDb = db; }
    LevelReading takeInputLevels() override { return inputMeter.take(); }
    float getOutputGainDb() const override { return outputGainDb.load(); }
    void setOutputGainDb (float db) override { outputGainDb = db; }
    LevelReading takeOutputLevels() override { return outputMeter.take(); }

    // 通知領域のアイコンのメーター用（入力・出力の欄のメーターとは別にためたピークと RMS。読むと 0 に戻る）
    LevelMeterTap::IconLevel takeInputIconLevel() { return inputMeter.takeIconLevel(); }
    LevelMeterTap::IconLevel takeOutputIconLevel() { return outputMeter.takeIconLevel(); }

    // 出力の保護（安全リミッター。設定メニューで切り替え、作動した回数をステータスバーに出す）
    OutputGuard& getOutputGuard() { return outputGuard; }

    juce::String getInputSubtitle() const override;
    juce::String getOutputSubtitle() const override;
    juce::String describeTotalLatency() const override;
    std::vector<MergedRack> getMergedRacks() const override;

    // 音声テスト（サイン波。TestTone）。入力（output = false）は入力ゲインの手前に、出力（output = true）はプラグインのあと・
    // 出力ゲインの手前に足す。保存はしない。鳴らす間は、ほかに何も鳴っていなくても APO が動くよう無音を流す
    void setTestToneOn (bool output, bool shouldBeOn);
    bool isTestToneOn (bool output) const { return (output ? outputTone : inputTone).isOn(); }
    void stopTestTones() { setTestToneOn (false, false); setTestToneOn (true, false); }

    //==============================================================================
    // ApoLink::Processor（音声スレッド）
    void Prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames) override;
    void Process (const float* input, float* output, uint32_t frames, uint32_t channels) override;

private:
    void handleAsyncUpdate() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void startLoadingPlugins();
    void finishLoadingPlugins();
    void buildAppRack (const juce::ValueTree& rackTree); // 預かった・読み込んだ中身からラックを作る（プラグインはあとで読み込む）
    void clearAppRacks();
    std::vector<PluginChain*> allChains(); // メインのラックとアプリ用ラックの並び
    void applyFormat (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames);
    void publishRacks();
    void updateSilentStream();
    void startLink(); // 今のデバイスと処理バッファの設定で APO とつなぐ

    PluginChain chain;
    std::vector<std::unique_ptr<AppRack>> appRacks;
    int nextRackId = 1;
    bool appRacksEnabled = false;
    std::vector<juce::ValueTree> heldAppRacks; // 機能が無効のあいだ預かっている AppRack の中身（親のないコピー）

    // 処理の形式（メッセージスレッド）
    uint32_t formatRate = 48000;
    uint32_t formatChannels = 2;
    uint32_t formatMaxFrames = 480;

    // APO から知らされた形式（音声スレッドが書く）
    std::atomic<uint32_t> linkRate { 0 };
    std::atomic<uint32_t> linkChannels { 0 };
    std::atomic<uint32_t> linkMaxFrames { 0 };

    // 音声スレッドが使うもの（processLock で守る）
    juce::SpinLock processLock;
    std::vector<AppRack*> activeRacks;
    juce::AudioBuffer<float> mainBuffer, rackBuffer;
    uint32_t processRate = 0;
    uint32_t processChannels = 0;
    uint32_t processMaxFrames = 0;

    // 音声スレッドだけが使う
    juce::SmoothedValue<float> inputGain, outputGain;
    uint32_t smoothingRate = 0;

    std::atomic<float> inputGainDb { 0.0f };
    std::atomic<float> outputGainDb { 0.0f };
    LevelMeterTap inputMeter;  // 入力の欄のメーター（入力ゲインのあと）
    LevelMeterTap outputMeter; // 出力の欄のメーター（出力ゲインのあと、出力の保護の前）
    OutputGuard outputGuard;   // 出力の保護（安全リミッター）
    TestTone inputTone, outputTone; // 音声テスト

    // 処理の負荷: Process にかかった時間の合計と、処理した音の長さの合計（音声スレッドが足し、checkOverload が差を見る）
    std::atomic<juce::int64> busyTicks { 0 }, processedFrames { 0 };

    // checkOverload の状態（メッセージスレッド）
    Overload overload;
    double overloadCheckedMs = 0.0, lastOverloadMs = 0.0, previewOverloadUntilMs = 0.0;
    juce::int64 lastBusyTicks = 0, lastProcessedFrames = 0;
    juce::uint64 lastUnderruns = 0, lastTimeouts = 0;
    bool overloadPrimed = false;
    unsigned dropHistory = 0; // 直近 4 回（2 秒）の、バッファ不足か時間切れが増えたか（1 ビットずつ）

    // プラグインを順に読み込む状態（メッセージスレッド）
    bool loadingPlugins = false;
    bool loadingOne = false; // 1 つ読み込んでいる最中（プラグインが初期化の中でメッセージを回しても、入れ子で次を始めない）
    int loadedCount = 0, loadTotal = 0;
    juce::uint32 lastLoadEndMs = 0; // 最後に 1 つ読み込み終わった時刻（次まで少し空けて、画面を描き直す）
    juce::StringArray loadErrors;

    juce::String endpointName, endpointGuid;
    int processingBufferMs = 0;
    bool processingBufferAuto = false;
    std::unique_ptr<ProcessingBuffer> processingBuffer; // link より先に宣言する（link が先に止まって消えるように）
    std::unique_ptr<pri::host::ApoLink> link;
    pri::host::SilentStream silentStream;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackEngine)
};
