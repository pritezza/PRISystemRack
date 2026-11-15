#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <thread>
#include <vector>

#include "HostCore/ApoLink.h"

// 処理バッファ（Cubase の ASIO-Guard のような先読み）。APO とのやり取りのスレッドと、プラグインを処理するスレッドを分ける。
//
// - Process（APO とのやり取りのスレッド）: 届いた音を入力の列に入れ、処理済みの列から同じ長さを取り出してすぐ返す（待たない）
// - 処理スレッド（MMCSS "Pro Audio"）: 入力の列から取り出して inner（RackEngine）に処理させ、処理済みの列に入れる
// - 処理済みの音は「バッファの量」だけ遅れて返る。代わりに、ときどき 1 周期に収まらない重いプラグイン
//   （Waves Clarity Vx など、数周期に 1 回まとめて計算するもの）でも、平均が間に合えば音が途切れない
// - 量は動かしたまま変えられる（つなぎ直さない）。増やすときはその分の無音を差し込み、減らすときはその分を捨てる
// - 差し込む量と捨てる量は、周期ごとに「列の中の量」と「今の量」の差から決め直す（予定を持ち越さない）。
//   重すぎて入力の列があふれ、入力を捨てたときも、捨てた分は数えないのでずれが残らない
//   （以前は「間に合わなかった分はあとで届くので捨てる」予定を積み上げていたため、捨てた入力の分の予定が永久に残り、
//   プラグインを外しても毎周期すべて捨てて無音になることがあった）
// - 間に合わなかった周期は無音を返す。手動のときは、あとから届いた分を捨てて量を保つ。
//   自動のときは、足りなかった分 + 余裕だけ量を増やす（すでに途切れているので、増やすことで新たな途切れは出ない）。
//   また、30 秒ずっと余裕が大きければ少しずつ減らす（捨てる部分がほぼ無音のときだけ捨てるので、ぷちっとならない）
class ProcessingBuffer final : public pri::host::ApoLink::Processor
{
public:
    static constexpr int maxMilliseconds = 200;
    static constexpr int minAutoMilliseconds = 2;

    ProcessingBuffer (pri::host::ApoLink::Processor& innerProcessor, int initialMilliseconds, bool autoAdjust);
    ~ProcessingBuffer() override;

    // どのスレッドから呼んでもよい
    void setTargetMilliseconds (int milliseconds);  // 手動のときの量（自動のときは、自動の出発点）
    void setAutoAdjust (bool shouldAutoAdjust);
    bool isAutoAdjust() const { return autoAdjust.load(); }
    double getCurrentMilliseconds() const;          // 今の量（自動のときは自動で決まった量）
    int getCurrentFrames() const { return currentFrames.load(); }
    juce::uint64 getUnderruns() const { return underruns.load(); }

    // ApoLink::Processor（APO とのやり取りのスレッド）
    void Prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames) override;
    void Process (const float* input, float* output, uint32_t frames, uint32_t channels) override;

private:
    void startWorker();
    void stopWorker();
    void workerLoop();
    void applyTarget();                                      // 手動の量に合わせる（やり取りのスレッド）
    void adjustAutomatically (int leftoverFrames, uint32_t frames); // 自動で減らすかを決める（やり取りのスレッド）
    bool skipQuietFrames (int frames);                        // 次の frames がほぼ無音なら捨てる

    pri::host::ApoLink::Processor& inner;

    std::atomic<int> targetMs;
    std::atomic<bool> autoAdjust;
    std::atomic<int> currentFrames { 0 }; // 今の量（フレーム数。やり取りのスレッドが書く）

    uint32_t rate = 0;
    std::atomic<uint32_t> rateForUi { 0 }; // 画面から今の量を ms で読むため
    uint32_t numChannels = 0;
    uint32_t maxBlockFrames = 0;

    // 入力の列（やり取りのスレッドが書き、処理スレッドが読む）と処理済みの列（その逆）。位置はフレーム単位、中身はインターリーブ
    std::unique_ptr<juce::AbstractFifo> inputFifo, outputFifo;
    std::vector<float> inputData, outputData;
    std::vector<float> workInput, workOutput; // 処理スレッドだけが使う

    juce::WaitableEvent wake;
    std::thread worker;
    std::atomic<bool> stopRequested { false };

    // やり取りのスレッドだけが使う
    // 列の中の量（入力の列に入れた量 − 処理済みの列から取り出した・捨てた量。処理スレッドが処理中の分も含む）。
    // 周期ごとに、取り出したあとにこれが currentFrames になるよう、無音の差し込みと捨てる量を決める
    juce::int64 framesInFlight = 0;
    juce::int64 quietSkipPending = 0; // 自動で減らす量（ほぼ無音のときに捨てる）
    int appliedTargetMs = -1;
    bool wasAuto = false;
    juce::int64 windowFrames = 0;   // 自動: 余裕を見ている期間の長さ
    int windowMinLeftover = 0;      // 自動: その期間で、取り出したあとに残っていた量の最小

    std::atomic<int> lostFrames { 0 }; // 処理スレッドが処理済みの列に入りきらずに捨てた量（やり取りのスレッドが引き取る）
    std::atomic<juce::uint64> underruns { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessingBuffer)
};
