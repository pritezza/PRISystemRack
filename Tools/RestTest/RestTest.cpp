// RestTest: 無音時の休止（PluginChain::setRestWhenSilent）を、音声デバイスを使わずに確かめる。
// 本物のプラグインを PluginChain に入れ、1 kHz の音 1 秒 → 無音 → 音、の順に流して、休止に入った時刻・休止中の出力・
// 音が戻ったときにすぐ処理したかを出す。
//
// 例: RestTest.exe "C:\Program Files\Common Files\VST3\PRI Resonance Suppressor.vst3" [無音の秒数]

#include <juce_audio_processors/juce_audio_processors.h>

#include "App/PluginChain.h"
#include "App/GuiResources.h"
#include "App/PluginScanner.h"
#include "App/ProcessingBuffer.h"

namespace
{
// PluginChain を ApoLink::Processor として使う（インターリーブ ⇔ チャンネルごと）
class ChainProcessor final : public pri::host::ApoLink::Processor
{
public:
    explicit ChainProcessor (PluginChain& chainToUse) : chain (chainToUse) {}

    void Prepare (uint32_t, uint32_t channels, uint32_t maxFrames) override { work.setSize ((int) channels, (int) maxFrames); }

    void Process (const float* input, float* output, uint32_t frames, uint32_t channels) override
    {
        for (uint32_t ch = 0; ch < channels; ++ch)
            for (uint32_t i = 0; i < frames; ++i)
                work.setSample ((int) ch, (int) i, input[i * channels + ch]);
        chain.process (work.getArrayOfWritePointers(), channels, frames, 48000);
        for (uint32_t ch = 0; ch < channels; ++ch)
            for (uint32_t i = 0; i < frames; ++i)
                output[i * channels + ch] = work.getSample ((int) ch, (int) i);
    }

private:
    PluginChain& chain;
    juce::AudioBuffer<float> work;
};

// 処理バッファの試験用: 素通しするが、重さ（1 回の処理にかける時間）と、1 回だけ長く止まるのを決められる
class FakeHeavyProcessor final : public pri::host::ApoLink::Processor
{
public:
    std::atomic<double> msPerCall { 0.0 };
    std::atomic<double> stallOnceMs { 0.0 };

    void Prepare (uint32_t, uint32_t, uint32_t) override {}

    void Process (const float* input, float* output, uint32_t frames, uint32_t channels) override
    {
        std::memcpy (output, input, (size_t) frames * channels * sizeof (float));
        const double wait = msPerCall.load() + stallOnceMs.exchange (0.0);
        const auto until = juce::Time::getMillisecondCounterHiRes() + wait;
        while (juce::Time::getMillisecondCounterHiRes() < until)
            juce::Thread::sleep (1);
    }
};

// --buffer-overload: 処理が実時間に追いつかない状態（重すぎる、長く止まる）のあと、軽くしたら音が戻るかを確かめる。
// 1 kHz を 10 ms（480 サンプル）ごとに流し、2 秒ふつう → 重い（または 1.5 秒止まる）→ 4 秒ふつう。
// 最後の 2 秒でバッファ不足が起きず、すべての周期に音があれば OK
bool runOverloadTest (bool autoBuffer, int bufferMs, bool hang)
{
    constexpr uint32_t rate = 48000, channels = 2, block = 480;
    FakeHeavyProcessor inner;
    ProcessingBuffer buffer (inner, bufferMs, autoBuffer);
    buffer.Prepare (rate, channels, block);

    std::vector<float> in ((size_t) block * channels), out ((size_t) block * channels);
    double phase = 0.0;
    const int normalBlocks = 200, heavyBlocks = 300, recoverBlocks = 400, checkBlocks = 200;
    juce::uint64 underrunsBeforeCheck = 0;
    int loudBlocksInCheck = 0;

    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (int b = 0; b < normalBlocks + heavyBlocks + recoverBlocks; ++b)
    {
        if (b == normalBlocks)
        {
            if (hang)
                inner.stallOnceMs = 1500.0;
            else
                inner.msPerCall = 15.0; // 10 ms の周期に 15 ms かかる（実時間の 1.5 倍）
        }
        if (b == normalBlocks + heavyBlocks)
            inner.msPerCall = 0.0;
        if (b == normalBlocks + heavyBlocks + recoverBlocks - checkBlocks)
            underrunsBeforeCheck = buffer.getUnderruns();

        for (uint32_t i = 0; i < block; ++i)
        {
            const float v = 0.25f * (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * 1000.0 / rate;
            for (uint32_t ch = 0; ch < channels; ++ch)
                in[i * channels + ch] = v;
        }
        buffer.Process (in.data(), out.data(), block, channels);

        if (b >= normalBlocks + heavyBlocks + recoverBlocks - checkBlocks)
        {
            float peak = 0.0f;
            for (auto s : out)
                peak = juce::jmax (peak, std::abs (s));
            if (peak > 0.1f)
                ++loudBlocksInCheck;
        }

        // 実際の周期に合わせる（10 ms ごと）
        const auto next = start + 10.0 * (b + 1);
        while (juce::Time::getMillisecondCounterHiRes() < next)
            juce::Thread::sleep (1);
    }

    const auto underrunsInCheck = buffer.getUnderruns() - underrunsBeforeCheck;
    const bool ok = underrunsInCheck == 0 && loudBlocksInCheck == checkBlocks;
    std::printf ("%s %s start %d ms: underruns total %llu, last 2 s %llu, blocks with sound in last 2 s %d / %d, now %.1f ms -> %s\n",
                 hang ? "hang 1.5 s" : "heavy 3 s", autoBuffer ? "auto  " : "manual", bufferMs,
                 (unsigned long long) buffer.getUnderruns(), (unsigned long long) underrunsInCheck, loudBlocksInCheck, checkBlocks,
                 buffer.getCurrentMilliseconds(), ok ? "OK" : "NG");
    return ok;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    std::setvbuf (stdout, nullptr, _IONBF, 0);

    if (argc >= 2 && juce::String (argv[1]) == "--buffer-overload")
    {
        bool allOk = true;
        for (bool hang : { false, true })
        {
            allOk &= runOverloadTest (true, 2, hang);
            allOk &= runOverloadTest (true, 200, hang);
            allOk &= runOverloadTest (false, 50, hang);
        }
        return allOk ? 0 : 1;
    }

    if (argc < 2)
    {
        std::printf ("usage: RestTest <plugin.vst3> [silence seconds] [--name <plugin name>] [--cached] [--bench <seconds> [--buffer <ms>] [--auto] [--silence]]\n"
                     "  --name   : 1 つのファイルに複数入っているとき（WaveShell など）に使うプラグインの名前\n"
                     "  --cached : 中のプラグインを全部作って調べる代わりに、本体が保存した調べた結果から選ぶ（使う 1 つだけを作る）\n"
                     "  --bench  : 休止は試さず、雑音を流して 10 ms（480 サンプル）ごとの処理時間を測る\n"
                     "  --buffer : 処理バッファを通す（始めの量）。--auto で自動調整、--silence で雑音の代わりに無音を流す\n"
                     "RestTest --buffer-overload : プラグインを使わず、処理バッファが重すぎる状態や長い停止から戻るかを確かめる\n");
        return 2;
    }

    const juce::File file (juce::String::fromUTF8 (argv[1]));
    double silenceSeconds = 6.0;
    juce::String wantedName;
    double benchSeconds = 0.0;
    int bufferMs = 0;
    bool autoBuffer = false, silenceBench = false, stateCheck = false, useCache = false, gdiRelease = false;
    for (int i = 2; i < argc; ++i)
    {
        const juce::String arg (juce::String::fromUTF8 (argv[i]));
        if (arg == "--cached")
            useCache = true;
        else if (arg == "--gdi-release")
            gdiRelease = true;
        else if (arg == "--name" && i + 1 < argc)
            wantedName = juce::String::fromUTF8 (argv[++i]);
        else if (arg == "--bench" && i + 1 < argc)
            benchSeconds = juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--buffer" && i + 1 < argc)
            bufferMs = juce::String (argv[++i]).getIntValue();
        else if (arg == "--auto")
            autoBuffer = true;
        else if (arg == "--silence")
            silenceBench = true;
        else if (arg == "--state-check")
            stateCheck = true;
        else
            silenceSeconds = arg.getDoubleValue();
    }

    constexpr uint32_t rate = 48000, channels = 2, block = 480;

    // --cached: ファイルの中のプラグインを全部作って調べる（findAllTypesForFile）代わりに、本体が保存した調べた結果
    // （PluginCache.xml）から選ぶ（本体と同じく、使う 1 つだけを作る）
    juce::OwnedArray<juce::PluginDescription> types;
    juce::Array<juce::PluginDescription> cached;
    if (useCache && PluginScanner::findCached (file, cached))
    {
        for (const auto& type : cached)
            types.add (new juce::PluginDescription (type));
    }
    else
    {
        juce::VST3PluginFormat format;
        format.findAllTypesForFile (types, file.getFullPathName());
    }
    if (types.isEmpty())
    {
        std::printf ("error: cannot load %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }

    const juce::PluginDescription* chosen = types[0];
    for (auto* type : types)
        if (wantedName.isNotEmpty() && type->name == wantedName)
            chosen = type;

    PluginChain chain;
    chain.prepare (rate, channels, block);
    juce::String error;
    const auto createStart = juce::Time::getMillisecondCounterHiRes();
    std::printf ("creating %s (uniqueId %08x)...\n", chosen->name.toRawUTF8(), (unsigned) chosen->uniqueId);
    auto* plugin = chain.addPlugin (*chosen, error);
    std::printf ("created in %.1f s  (GDI %d, USER %d)\n", (juce::Time::getMillisecondCounterHiRes() - createStart) / 1000.0,
                 GuiResources::gdiObjects(), GuiResources::userObjects());

    // --gdi-release: 作ったプラグインを外して、描画資源が戻るかを見る
    if (gdiRelease && plugin != nullptr)
    {
        chain.removePlugin (plugin->id);
        std::printf ("removed: GDI %d, USER %d\n", GuiResources::gdiObjects(), GuiResources::userObjects());
        juce::Thread::sleep (2000);
        std::printf ("2 s later: GDI %d, USER %d\n", GuiResources::gdiObjects(), GuiResources::userObjects());
        return 0;
    }
    if (plugin == nullptr)
    {
        std::printf ("error: %s\n", error.toRawUTF8());
        return 1;
    }

    std::printf ("plugin : %s  latency %d smp  tail %.2f s\n", plugin->getDisplayName().toRawUTF8(),
                 plugin->instance->getLatencySamples(), plugin->instance->getTailLengthSeconds());

    juce::AudioBuffer<float> buffer ((int) channels, (int) block);

    // --state-check: 音を流す前後でプラグインの状態のデータが変わるかを見る
    // （プリセットの「保存していない変更がある」の判定は、状態のデータを比べるため）
    if (stateCheck)
    {
        auto getState = [plugin]
        {
            juce::MemoryBlock block;
            plugin->instance->getStateInformation (block);
            return block;
        };
        juce::Random random;
        auto run = [&] (double seconds)
        {
            for (int b = 0; b < (int) (seconds * rate / block); ++b)
            {
                for (int ch = 0; ch < (int) channels; ++ch)
                    for (int i = 0; i < (int) block; ++i)
                        buffer.setSample (ch, i, (random.nextFloat() * 2.0f - 1.0f) * 0.1f);
                chain.process (buffer.getArrayOfWritePointers(), channels, block, rate);
            }
        };

        const auto first = getState();
        const auto again = getState();
        run (3.0);
        const auto afterAudio = getState();
        run (3.0);
        const auto later = getState();
        std::printf ("state size %d bytes | same when read twice: %s | same after 3 s of audio: %s | same after 3 s more: %s\n",
                     (int) first.getSize(), first == again ? "yes" : "NO", first == afterAudio ? "yes" : "NO", afterAudio == later ? "yes" : "NO");

        // 読み込み直し（プリセットの読み込みと同じ）: 書き出した状態を戻して、すぐ・メッセージを回したあと・音を流したあとに読む
        plugin->instance->setStateInformation (later.getData(), (int) later.getSize());
        const auto rightAfterSet = getState();
        juce::Thread::sleep (500);
        const auto afterMessages = getState();
        run (3.0);
        const auto afterSetAndAudio = getState();
        std::printf ("after setState: same right away: %s | after 0.5 s: %s | after 3 s of audio: %s | stable after that: %s\n",
                     rightAfterSet == later ? "yes" : "NO", afterMessages == later ? "yes" : "NO",
                     afterSetAndAudio == later ? "yes" : "NO", afterSetAndAudio == afterMessages ? "yes" : "NO");
        return 0;
    }

    // 処理時間を測る: 雑音を流し、1 周期（10 ms）ごとの時間を集める。APO の待ち時間の上限は周期の 60%（6 ms）。
    // --buffer を付けると処理バッファを通し、APO に返すまでの時間（Process の時間）を測る
    if (benchSeconds > 0.0)
    {
        ChainProcessor direct (chain);
        std::unique_ptr<ProcessingBuffer> processingBuffer;
        pri::host::ApoLink::Processor* processor = &direct;
        if (bufferMs > 0 || autoBuffer)
        {
            processingBuffer = std::make_unique<ProcessingBuffer> (direct, bufferMs, autoBuffer);
            processor = processingBuffer.get();
        }
        processor->Prepare (rate, channels, block);
        std::vector<float> interleavedIn ((size_t) block * channels), interleavedOut ((size_t) block * channels);

        juce::Random random;
        std::vector<double> times;
        const int blocks = (int) (benchSeconds * rate / block);
        for (int b = 0; b < blocks; ++b)
        {
            for (auto& sample : interleavedIn)
                sample = silenceBench ? 0.0f : (random.nextFloat() * 2.0f - 1.0f) * 0.1f;

            const auto start = juce::Time::getMillisecondCounterHiRes();
            processor->Process (interleavedIn.data(), interleavedOut.data(), block, channels);
            times.push_back (juce::Time::getMillisecondCounterHiRes() - start);

            // 実際の周期に合わせて 10 ms ごとに処理する（まとめて流すと結果が変わるプラグインがあるため）
            const auto spent = juce::Time::getMillisecondCounterHiRes() - start;
            if (spent < 10.0)
                juce::Thread::sleep ((int) (10.0 - spent));
        }

        auto sorted = times;
        std::sort (sorted.begin(), sorted.end());
        double sum = 0.0;
        for (auto t : times)
            sum += t;
        const auto over6 = std::count_if (times.begin(), times.end(), [] (double t) { return t > 6.0; });
        std::printf ("blocks %d  avg %.2f ms  median %.2f ms  p99 %.2f ms  max %.2f ms  over 6 ms: %d (%.1f%%)\n",
                     (int) times.size(), sum / (double) times.size(), sorted[sorted.size() / 2],
                     sorted[(size_t) ((double) sorted.size() * 0.99)], sorted.back(), (int) over6, 100.0 * (double) over6 / (double) times.size());

        // 6 ms を超えた周期が続くか、ときどきか（最初の 20 個の位置）
        juce::String where;
        int shown = 0;
        for (size_t i = 0; i < times.size() && shown < 20; ++i)
            if (times[i] > 6.0)
            {
                where << juce::String (i) << "(" << juce::String (times[i], 1) << ") ";
                ++shown;
            }
        std::printf ("over 6 ms at block: %s\n", where.toRawUTF8());
        if (processingBuffer != nullptr)
            std::printf ("buffer start %d ms%s: underruns %llu, now %.1f ms\n", bufferMs, autoBuffer ? " (auto)" : "",
                         (unsigned long long) processingBuffer->getUnderruns(), processingBuffer->getCurrentMilliseconds());
        return 0;
    }

    PluginChain::setRestWhenSilent (true);
    double phase = 0.0;
    auto fill = [&] (bool tone)
    {
        for (int i = 0; i < (int) block; ++i)
        {
            const float v = tone ? 0.25f * (float) std::sin (phase) : 0.0f;
            phase += juce::MathConstants<double>::twoPi * 1000.0 / rate;
            for (int ch = 0; ch < (int) channels; ++ch)
                buffer.setSample (ch, i, v);
        }
    };

    const int toneBlocks = (int) (1.0 * rate / block);
    const int silentBlocks = (int) (silenceSeconds * rate / block);
    double restStartedAt = -1.0, lastOutputAt = -1.0;
    float peakWhileResting = 0.0f;

    for (int b = 0; b < toneBlocks + silentBlocks + toneBlocks; ++b)
    {
        const bool tone = b < toneBlocks || b >= toneBlocks + silentBlocks;
        fill (tone);
        chain.process (buffer.getArrayOfWritePointers(), channels, block, rate);

        const double t = (double) (b + 1) * block / rate;
        const float peak = buffer.getMagnitude (0, (int) block);
        const bool resting = plugin->resting.load();

        if (! tone && peak > 1.0e-6f)
            lastOutputAt = t;
        if (resting && restStartedAt < 0.0)
            restStartedAt = t;
        if (resting)
            peakWhileResting = juce::jmax (peakWhileResting, peak);

        // 音が戻った最初の周期
        if (b == toneBlocks + silentBlocks)
            std::printf ("tone back at %.2f s: resting=%d output peak=%.3f\n", t, resting ? 1 : 0, peak);
    }

    std::printf ("silence starts at 1.00 s\n");
    std::printf ("last non-silent output during silence: %.2f s\n", lastOutputAt);
    std::printf ("rest started at: %.2f s (%.2f s after the silence started)\n", restStartedAt, restStartedAt - 1.0);
    std::printf ("peak while resting: %.7f\n", peakWhileResting);
    return 0;
}
