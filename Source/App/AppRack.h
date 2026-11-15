#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "HostCore/ProcessCapture.h"
#include "HostCore/ProcessTools.h"
#include "RackModel.h"

#include <map>

// アプリ用ラック: 指定したアプリ（exe）が再生している音だけを WASAPI Process Loopback で取り込み、
// 自分のプラグインの並びで処理して、メインのラックの出力の手前（メインのプラグインのあと、出力ゲインの前）に足す。
//
// - 1 秒ごとにプロセスを調べ、対象 exe の根元のプロセスごとに取り込む（子プロセスはツリーごと取り込まれる）。
//   アプリが起動していなければ待ち、起動し直したらつなぎ直す。取り込みの準備は取り込みスレッドで行い（画面は止めない）、
//   失敗したプロセスは 5 秒待ってから試し直す
// - 取り込んだ音は FIFO に入れ、メインの処理（APO からの依頼）の中で render が周期ごとに取り出す。
//   対象アプリが別のデバイス（CABLE Input など）で鳴っていると時計がずれるので、FIFO の量が一定になるよう
//   読み出しの速さを少しだけ（±0.5% 以内）変えて合わせる
// - 対象アプリがどのデバイスに音を出しているかを 2 秒ごとに調べる。メインのデバイスなら二重に聞こえる
class AppRack final : public RackModel,
                      public juce::ChangeBroadcaster,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    struct Status
    {
        juce::StringArray runningTargets; // 取り込んでいる exe
        juce::StringArray waitingTargets; // 起動を待っている exe
        int processCount = 0;             // 取り込んでいるプロセスツリーのプロセス数
        juce::String captureError;
        juce::StringArray activeOutputs; // 対象アプリがいま音を出しているデバイスの名前
        bool doubled = false;            // メインのデバイスに直接出している（元の音も聞こえる）
        double delayMs = 0.0;            // 合流までのおおよその遅れ
    };

    // mainEndpointGuid: メインのラックが処理している再生デバイス（二重に聞こえるかの判定に使う）
    // bufferLatency: 処理バッファの遅れ（合流したあとも通るので、レイテンシーの表示に足す）
    AppRack (int id, std::function<juce::String()> mainEndpointGuid, std::function<int()> bufferLatency);
    ~AppRack() override;

    int getId() const { return id; }
    juce::String getName() const { return name; }
    void setName (const juce::String& newName);
    juce::StringArray getTargets() const { return targets; }
    void setTargets (const juce::StringArray& newTargets);
    const Status& getStatus() const { return status; }

    // 処理の形式（メッセージスレッド）。変わったら取り込みもつなぎ直す
    void prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames);

    // 音声スレッド: この周期の処理済みの音を out に書く（音がなければ無音）
    void render (float* const* out, uint32_t channels, uint32_t frames, uint32_t sampleRate);

    void saveState (juce::ValueTree& parent) const; // parent に AppRack の子を足す
    void restoreState (const juce::ValueTree& rackTree, juce::StringArray& errors, bool loadLater = false); // loadLater: PluginChain::restoreState と同じ

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
    juce::String getInputSubtitle() const override;
    juce::String getOutputSubtitle() const override;
    bool outputIsMerge() const override { return true; }
    juce::String describeTotalLatency() const override;

private:
    struct Source;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scanProcesses();
    void updateOutputs();
    void stopAllSources();
    void publishSources();

    const int id;
    std::function<juce::String()> mainEndpointGuid;
    std::function<int()> bufferLatency;
    juce::String name;
    juce::StringArray targets;
    PluginChain chain;
    Status status;
    int timerTicks = 0;

    // 取り込み（メッセージスレッドが持つ）
    std::vector<std::shared_ptr<Source>> sources;
    std::vector<uint32_t> currentRoots;

    // 取り込みに失敗したプロセス（根元の PID ごと）。retryDelayMs 待ってから試し直す
    struct Failure
    {
        juce::String message;    // "exe: 理由"
        juce::uint32 retryAtMs = 0; // juce::Time::getMillisecondCounter() の値
    };
    static constexpr juce::uint32 retryDelayMs = 5000;
    std::map<uint32_t, Failure> failures;

    // 前に調べた Windows Audio の状態（サービスと audiodg.exe の PID）。Windows Audio が再起動すると
    // （再生デバイスの［有効］・［無効］の切り替えなど）、それより前に始めた取り込みは、エラーを返さず、
    // 届き続けるパケットの中身が 0 になるだけ（対象アプリが黙っているときと見分けられない）。PID が変わったら作り直す
    pri::host::AudioServiceState audioService;

    // 対象アプリが実際に音を出していたデバイス（GUID ごと）。最後に音が出てから heardMemoryMs までは出力先として扱う
    struct HeardOutput
    {
        juce::String name;
        juce::uint32 lastHeardMs = 0;
    };
    static constexpr juce::uint32 heardMemoryMs = 10000;
    std::map<juce::String, HeardOutput> heardOutputs;

    // 音声スレッドが使うもの（renderLock で守る）
    juce::SpinLock renderLock;
    std::vector<Source*> activeSources;
    juce::AudioBuffer<float> mixBuffer, readBuffer, sourceBuffer;
    uint32_t preparedRate = 48000;
    uint32_t preparedChannels = 2;
    uint32_t preparedMaxFrames = 480;

    juce::SmoothedValue<float> inputGain, outputGain;
    uint32_t smoothingRate = 0;
    std::atomic<float> inputGainDb { 0.0f };
    std::atomic<float> outputGainDb { 0.0f };
    LevelMeterTap inputMeter;  // 入力の欄のメーター（入力ゲインのあと）
    LevelMeterTap outputMeter; // 出力の欄のメーター（出力ゲインのあと）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRack)
};
