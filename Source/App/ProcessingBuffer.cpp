#include "ProcessingBuffer.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace
{
// 自動で減らすときに「ほぼ無音」とみなす大きさ（-70 dB 未満）
constexpr float quietThreshold = 3.0e-4f;

// フレーム単位の FIFO の区間（start, size）を、インターリーブのデータとして写す
void copyIn (std::vector<float>& ring, int start, int size, const float* source, uint32_t channels)
{
    if (size > 0)
        std::memcpy (ring.data() + (size_t) start * channels, source, (size_t) size * channels * sizeof (float));
}

void copyOut (const std::vector<float>& ring, int start, int size, float* dest, uint32_t channels)
{
    if (size > 0)
        std::memcpy (dest, ring.data() + (size_t) start * channels, (size_t) size * channels * sizeof (float));
}

bool isQuiet (const std::vector<float>& ring, int start, int size, uint32_t channels)
{
    const auto* data = ring.data() + (size_t) start * channels;
    for (size_t i = 0; i < (size_t) size * channels; ++i)
        if (std::abs (data[i]) >= quietThreshold)
            return false;
    return true;
}

int msToFrames (double ms, uint32_t sampleRate)
{
    return (int) std::lround (ms * sampleRate / 1000.0);
}
} // namespace

ProcessingBuffer::ProcessingBuffer (pri::host::ApoLink::Processor& innerProcessor, int initialMilliseconds, bool shouldAutoAdjust)
    : inner (innerProcessor),
      targetMs (juce::jlimit (0, maxMilliseconds, initialMilliseconds)),
      autoAdjust (shouldAutoAdjust)
{
}

ProcessingBuffer::~ProcessingBuffer()
{
    stopWorker();
}

void ProcessingBuffer::setTargetMilliseconds (int milliseconds)
{
    targetMs = juce::jlimit (0, maxMilliseconds, milliseconds);
}

void ProcessingBuffer::setAutoAdjust (bool shouldAutoAdjust)
{
    autoAdjust = shouldAutoAdjust;
}

double ProcessingBuffer::getCurrentMilliseconds() const
{
    const auto sampleRate = rateForUi.load();
    return sampleRate > 0 ? 1000.0 * currentFrames.load() / sampleRate : (double) targetMs.load();
}

//==============================================================================
void ProcessingBuffer::Prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames)
{
    // 形式が変わるのはまれなので、処理スレッドを止めて作り直す（ここだけはメモリを確保する）
    stopWorker();

    rate = sampleRate;
    rateForUi = sampleRate;
    numChannels = channels;
    maxBlockFrames = maxFrames;

    const int capacity = msToFrames (maxMilliseconds, sampleRate) + (int) maxFrames * 16; // 最大の量 + 16 周期ぶんの余り
    inputFifo = std::make_unique<juce::AbstractFifo> (capacity);
    outputFifo = std::make_unique<juce::AbstractFifo> (capacity);
    inputData.assign ((size_t) capacity * channels, 0.0f);
    outputData.assign ((size_t) capacity * channels, 0.0f);
    workInput.assign ((size_t) maxFrames * channels, 0.0f);
    workOutput.assign ((size_t) maxFrames * channels, 0.0f);

    // 始めの量の無音を先に入れておく（中身は 0 で確保済み）
    const bool isAuto = autoAdjust.load();
    const int startMs = isAuto ? juce::jmax (minAutoMilliseconds, targetMs.load()) : targetMs.load();
    const int startFrames = msToFrames (startMs, sampleRate);
    int start1, size1, start2, size2;
    outputFifo->prepareToWrite (startFrames, start1, size1, start2, size2);
    outputFifo->finishedWrite (size1 + size2);

    currentFrames = startFrames;
    framesInFlight = startFrames;
    lostFrames = 0;
    appliedTargetMs = targetMs.load();
    wasAuto = isAuto;
    quietSkipPending = 0;
    windowFrames = 0;
    windowMinLeftover = std::numeric_limits<int>::max();

    inner.Prepare (sampleRate, channels, maxFrames);
    startWorker();
}

void ProcessingBuffer::Process (const float* input, float* output, uint32_t frames, uint32_t channels)
{
    if (inputFifo == nullptr)
    {
        inner.Process (input, output, frames, channels); // 準備前（処理スレッドはまだない）は、このスレッドでそのまま処理する
        return;
    }
    if (channels != numChannels || frames > maxBlockFrames)
    {
        // 形式違い（来ないはず）。処理スレッドが inner を処理している最中かもしれないので、ここで inner を呼ぶと
        // 2 つのスレッドから同時に処理することになる。その周期は無音を返す
        std::memset (output, 0, (size_t) frames * channels * sizeof (float));
        return;
    }

    // 届いた音を入力の列へ（入りきらないのは処理スレッドが大きく遅れているとき。その分は捨てる。
    // 捨てた分は列の中の量に数えないので、その分はあとで無音を差し込んで量を保つ）
    {
        int start1, size1, start2, size2;
        inputFifo->prepareToWrite ((int) frames, start1, size1, start2, size2);
        copyIn (inputData, start1, size1, input, channels);
        copyIn (inputData, start2, size2, input + (size_t) size1 * channels, channels);
        inputFifo->finishedWrite (size1 + size2);
        framesInFlight += size1 + size2;
    }
    wake.signal();

    // 処理スレッドが処理済みの列に入りきらずに捨てた分
    framesInFlight -= lostFrames.exchange (0, std::memory_order_relaxed);

    // 手動と自動の切り替え、手動の量の変更
    const bool isAuto = autoAdjust.load (std::memory_order_relaxed);
    if (isAuto != wasAuto)
    {
        wasAuto = isAuto;
        quietSkipPending = 0;
        windowFrames = 0;
        windowMinLeftover = std::numeric_limits<int>::max();
        appliedTargetMs = isAuto ? appliedTargetMs : -1; // 手動に戻したら、その時点の手動の量に合わせる
        if (isAuto && currentFrames.load() < msToFrames (minAutoMilliseconds, rate))
            currentFrames = msToFrames (minAutoMilliseconds, rate);
    }
    if (! isAuto)
        applyTarget();

    // 自動で減らす分は、捨てる部分がほぼ無音のときだけ捨てる（1 周期ぶんずつ）
    if (quietSkipPending > 0)
    {
        const int skip = (int) juce::jmin (quietSkipPending, (juce::int64) frames);
        if (skipQuietFrames (skip))
        {
            quietSkipPending -= skip;
            currentFrames -= skip;
            framesInFlight -= skip;
        }
    }

    // この周期で列から取り出す量（捨てる分 + 返す分）は、取り出したあとに列の中の量が今の量（currentFrames）になるように決める。
    // 足りなければ先に無音を返し（量を増やした、入力を捨てた）、多すぎれば古い方から捨てる（量を減らした、間に合わなかった分があとで届いた）。
    // 前の周期までの差し込み・捨てる予定を持ち越さず、毎回ここで決め直すので、ずれが残らない
    const juce::int64 toTake = framesInFlight - currentFrames.load();
    const int silenceFrames = (int) juce::jlimit ((juce::int64) 0, (juce::int64) frames, (juce::int64) frames - toTake);
    const juce::int64 toDrop = juce::jmax ((juce::int64) 0, toTake - (juce::int64) frames);

    std::memset (output, 0, (size_t) silenceFrames * channels * sizeof (float));

    if (toDrop > 0)
    {
        const int drop = (int) juce::jmin ((juce::int64) outputFifo->getNumReady(), toDrop);
        int start1, size1, start2, size2;
        outputFifo->prepareToRead (drop, start1, size1, start2, size2);
        outputFifo->finishedRead (size1 + size2);
        framesInFlight -= size1 + size2;
    }

    // 処理済みの音を返す
    const int needed = (int) frames - silenceFrames;
    const int ready = outputFifo->getNumReady();
    int start1, size1, start2, size2;
    outputFifo->prepareToRead (needed, start1, size1, start2, size2);
    float* dest = output + (size_t) silenceFrames * channels;
    copyOut (outputData, start1, size1, dest, channels);
    copyOut (outputData, start2, size2, dest + (size_t) size1 * channels, channels);
    outputFifo->finishedRead (size1 + size2);

    const int got = size1 + size2;
    framesInFlight -= got;

    if (got < needed)
    {
        // 間に合わなかった。足りない分は無音にする（その分はあとで届いたら捨てる）
        const int missing = needed - got;
        std::memset (dest + (size_t) got * channels, 0, (size_t) missing * channels * sizeof (float));
        underruns.fetch_add (1, std::memory_order_relaxed);

        if (isAuto)
        {
            // 足りなかった分 + 余裕 2 ms だけ増やす（上限まで）。足りなかった分はあとで届いて、そのまま遅れになる
            currentFrames = juce::jmin (msToFrames (maxMilliseconds, rate), currentFrames.load() + missing + msToFrames (2.0, rate));
            quietSkipPending = 0;
            windowFrames = 0;
            windowMinLeftover = std::numeric_limits<int>::max();
        }
        return;
    }

    if (isAuto)
        adjustAutomatically (ready - needed, frames);
}

void ProcessingBuffer::applyTarget()
{
    const int target = targetMs.load (std::memory_order_relaxed);
    if (target == appliedTargetMs)
        return;

    // 量を変えるだけ。増やした分の無音の差し込みと、減らした分を捨てるのは Process で決める
    appliedTargetMs = target;
    currentFrames = msToFrames (target, rate);
}

void ProcessingBuffer::adjustAutomatically (int leftoverFrames, uint32_t frames)
{
    // 取り出したあとに残っていた量（＝処理がどれだけ先に進んでいたか）の最小を、30 秒ごとに見る
    windowMinLeftover = juce::jmin (windowMinLeftover, leftoverFrames);
    windowFrames += frames;

    if (windowFrames < (juce::int64) rate * 30)
        return;

    // 30 秒のあいだ一度も残りが 3 ms を切らなかったら、余っていた分の半分（1 回に最大 5 ms）だけ減らす
    const int margin = msToFrames (3.0, rate);
    if (windowMinLeftover > margin && quietSkipPending == 0)
    {
        int reduce = juce::jmin ((windowMinLeftover - margin) / 2, msToFrames (5.0, rate));
        reduce = juce::jmin (reduce, currentFrames.load() - msToFrames (minAutoMilliseconds, rate));
        if (reduce > 0)
            quietSkipPending = reduce;
    }

    windowFrames = 0;
    windowMinLeftover = std::numeric_limits<int>::max();
}

bool ProcessingBuffer::skipQuietFrames (int frames)
{
    if (outputFifo->getNumReady() < frames)
        return false;

    int start1, size1, start2, size2;
    outputFifo->prepareToRead (frames, start1, size1, start2, size2);
    if (! isQuiet (outputData, start1, size1, numChannels) || ! isQuiet (outputData, start2, size2, numChannels))
        return false; // 音がある。読んだことにはしない（次の機会を待つ）

    outputFifo->finishedRead (size1 + size2);
    return true;
}

//==============================================================================
void ProcessingBuffer::startWorker()
{
    stopRequested = false;
    worker = std::thread ([this] { workerLoop(); });
}

void ProcessingBuffer::stopWorker()
{
    stopRequested = true;
    wake.signal();
    if (worker.joinable())
        worker.join();
}

void ProcessingBuffer::workerLoop()
{
    // APO とのやり取りのスレッドと同じく、音声用の優先度で動かす
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);

    while (! stopRequested.load())
    {
        wake.wait (50);

        // 届いている分を、1 周期ぶん（maxBlockFrames）ずつ順に処理する
        while (! stopRequested.load() && inputFifo->getNumReady() > 0)
        {
            const int frames = juce::jmin (inputFifo->getNumReady(), (int) maxBlockFrames);

            int start1, size1, start2, size2;
            inputFifo->prepareToRead (frames, start1, size1, start2, size2);
            copyOut (inputData, start1, size1, workInput.data(), numChannels);
            copyOut (inputData, start2, size2, workInput.data() + (size_t) size1 * numChannels, numChannels);
            inputFifo->finishedRead (size1 + size2);

            inner.Process (workInput.data(), workOutput.data(), (uint32_t) frames, numChannels);

            outputFifo->prepareToWrite (frames, start1, size1, start2, size2);
            copyIn (outputData, start1, size1, workOutput.data(), numChannels);
            copyIn (outputData, start2, size2, workOutput.data() + (size_t) size1 * numChannels, numChannels);
            outputFifo->finishedWrite (size1 + size2);
            if (size1 + size2 < frames)
                lostFrames.fetch_add (frames - (size1 + size2), std::memory_order_relaxed); // 入りきらなかった（来ないはず）
        }
    }

    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics (mmcss);
}
