#include "AppRack.h"

#include "AppLog.h"

#include "HostCore/ProcessTools.h"

#include <array>
#include <cmath>
#include <map>
#include <set>

namespace
{
std::wstring toWide (const juce::String& text)
{
    return std::wstring (text.toWideCharPointer());
}
} // namespace

//==============================================================================
// 取り込み 1 本分（根元のプロセス 1 つとその子プロセス）
struct AppRack::Source
{
    Source (uint32_t processId, const juce::String& exeName, uint32_t channelCount, uint32_t sampleRate)
        : pid (processId),
          exe (exeName),
          channels (channelCount),
          fifo ((int) sampleRate / 2), // 0.5 秒
          buffer ((int) channelCount, (int) sampleRate / 2)
    {
        buffer.clear();
    }

    ~Source()
    {
        capture.Stop(); // 取り込みスレッドを止めてから片付ける
    }

    // 取り込みスレッド。溢れた分は捨てる（読み出し側が速さを上げて追いつく）
    void write (const float* interleaved, uint32_t frames, uint32_t inputChannels)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite ((int) frames, start1, size1, start2, size2);

        auto copy = [&] (int start, int count, int offset)
        {
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                auto* dest = buffer.getWritePointer ((int) ch, start);
                const uint32_t source = juce::jmin (ch, inputChannels - 1);
                for (int f = 0; f < count; ++f)
                    dest[f] = interleaved[(size_t) (offset + f) * inputChannels + source];
            }
        };
        copy (start1, size1, 0);
        copy (start2, size2, size1);
        fifo.finishedWrite (size1 + size2);
    }

    // 音声スレッド: frames 分を読み出して out に足す。読み出せなければ false（無音）
    bool addTo (float* const* out, uint32_t frames, uint32_t maxFrames, uint32_t sampleRate,
                juce::AudioBuffer<float>& read, juce::AudioBuffer<float>& temp)
    {
        // 取り込みは 10 ms ほどの塊で届くので、2 周期ぶん溜めてから読み始める
        const int target = (int) maxFrames * 2;
        const int ready = fifo.getNumReady();
        lastReady = ready;

        if (! primed)
        {
            if (ready < target)
                return false;
            primed = true;
            smoothedError = 0.0;
        }

        // 溜まり具合が目標から外れた分を、2 秒ほどかけて読み出しの速さで戻す（時計のずれの吸収）
        smoothedError += 0.01 * ((double) (ready - target) - smoothedError);
        const double ratio = 1.0 + juce::jlimit (-0.005, 0.005, smoothedError / (2.0 * (double) sampleRate));

        const int needed = (int) std::ceil ((double) frames * ratio) + 8;
        if (ready < needed || needed > read.getNumSamples())
        {
            primed = false; // 足りなくなったら溜め直す（ここで一度途切れる）
            return false;
        }

        int start1, size1, start2, size2;
        fifo.prepareToRead (needed, start1, size1, start2, size2);

        int used = 0;
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            read.copyFrom ((int) ch, 0, buffer, (int) ch, start1, size1);
            if (size2 > 0)
                read.copyFrom ((int) ch, size1, buffer, (int) ch, start2, size2);

            used = interpolators[ch].process (ratio, read.getReadPointer ((int) ch), temp.getWritePointer ((int) ch), (int) frames);
            juce::FloatVectorOperations::add (out[ch], temp.getReadPointer ((int) ch), (int) frames);
        }

        fifo.finishedRead (juce::jmin (used, needed));
        return true;
    }

    const uint32_t pid;
    const juce::String exe;
    const uint32_t channels;
    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> buffer;
    std::array<juce::WindowedSincInterpolator, 8> interpolators;
    bool primed = false;
    double smoothedError = 0.0;
    std::atomic<int> lastReady { 0 };
    pri::host::ProcessCapture capture;
};

//==============================================================================
AppRack::AppRack (int rackId, std::function<juce::String()> mainEndpointGuidToUse, std::function<int()> bufferLatencyToUse)
    : id (rackId),
      mainEndpointGuid (std::move (mainEndpointGuidToUse)),
      bufferLatency (std::move (bufferLatencyToUse))
{
    name = TRANS ("App rack %number%").replace ("%number%", juce::String (rackId));
    chain.addChangeListener (this);
    prepare (preparedRate, preparedChannels, preparedMaxFrames);
    startTimer (1000);
}

AppRack::~AppRack()
{
    stopTimer();
    chain.removeChangeListener (this);
    stopAllSources();
}

void AppRack::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sendChangeMessage(); // チェーンの変更も、このラックの変更として知らせる
}

void AppRack::setName (const juce::String& newName)
{
    if (newName == name)
        return;
    name = newName;
    sendChangeMessage();
}

void AppRack::setTargets (const juce::StringArray& newTargets)
{
    targets = newTargets;
    targets.removeEmptyStrings();
    targets.removeDuplicates (true);
    scanProcesses();
    sendChangeMessage();
}

juce::String AppRack::getInputSubtitle() const
{
    if (targets.isEmpty())
        return TRANS ("No target apps yet (choose them above)");
    return TRANS ("Audio of %apps% (Process Loopback)").replace ("%apps%", targets.joinIntoString (", "));
}

juce::String AppRack::getOutputSubtitle() const
{
    return TRANS ("Into the main rack, before its output");
}

// 合流するまでの遅れ: このラックのプラグイン ＋ 取り込み（FIFO と取り込みの塊）＋ 処理バッファ
juce::String AppRack::describeTotalLatency() const
{
    const double rate = (double) preparedRate;
    const auto ms = [rate] (int samples) { return rate > 0.0 ? 1000.0 * samples / rate : 0.0; };

    const int pluginSamples = chain.getTotalLatencySamples();
    const int bufferSamples = bufferLatency != nullptr ? bufferLatency() : 0;
    const double captureMs = status.delayMs;
    const double totalMs = ms (pluginSamples) + captureMs + ms (bufferSamples);

    juce::StringArray parts;
    parts.add (TRANS ("plugins %latency%").replace ("%latency%", juce::String (ms (pluginSamples), 1) + " ms"));
    parts.add (TRANS ("capture about %latency%").replace ("%latency%", juce::String (juce::roundToInt (captureMs)) + " ms"));
    if (bufferSamples > 0)
        parts.add (TRANS ("buffer %latency%").replace ("%latency%", juce::String (ms (bufferSamples), 1) + " ms"));

    return TRANS ("Total latency: about %latency%").replace ("%latency%", juce::String (totalMs, 1) + " ms")
         + "  (" + parts.joinIntoString (" + ") + ")";
}

//==============================================================================
void AppRack::prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames)
{
    if (sampleRate == 0 || channels == 0 || maxFrames == 0)
        return;

    chain.prepare (sampleRate, channels, maxFrames);

    const bool changed = sampleRate != preparedRate || channels != preparedChannels || maxFrames != preparedMaxFrames
                      || mixBuffer.getNumSamples() == 0;
    if (! changed)
        return;

    // 形式が変わったら、取り込みは新しい形式でつなぎ直す（次の scanProcesses で）
    stopAllSources();

    const int readSize = (int) std::ceil (maxFrames * 1.01) + 16;
    juce::AudioBuffer<float> mix ((int) channels, (int) maxFrames), read ((int) channels, readSize), temp ((int) channels, (int) maxFrames);
    {
        const juce::SpinLock::ScopedLockType lock (renderLock);
        std::swap (mixBuffer, mix);
        std::swap (readBuffer, read);
        std::swap (sourceBuffer, temp);
        preparedRate = sampleRate;
        preparedChannels = channels;
        preparedMaxFrames = maxFrames;
    }

    scanProcesses();
}

void AppRack::stopAllSources()
{
    {
        const juce::SpinLock::ScopedLockType lock (renderLock);
        activeSources.clear();
    }
    sources.clear(); // それぞれ取り込みを止めてから消える
}

void AppRack::publishSources()
{
    std::vector<Source*> list;
    for (const auto& source : sources)
        list.push_back (source.get());

    const juce::SpinLock::ScopedLockType lock (renderLock);
    activeSources.swap (list);
}

void AppRack::timerCallback()
{
    scanProcesses();
    if (++timerTicks % 2 == 0)
        updateOutputs();
}

// 対象 exe の根元のプロセスを探し、取り込みを増やしたり減らしたりする
void AppRack::scanProcesses()
{
    std::map<uint32_t, juce::String> wanted; // 根元の PID -> exe 名
    status.runningTargets.clear();
    status.waitingTargets.clear();

    for (const auto& target : targets)
    {
        const auto roots = pri::host::FindRootProcesses (toWide (target));
        if (roots.empty())
            status.waitingTargets.add (target);
        else
            status.runningTargets.add (target);
        for (const auto root : roots)
            wanted[root] = target;
    }

    bool changed = false;
    const auto now = juce::Time::getMillisecondCounter();
    using State = pri::host::ProcessCapture::State;

    // Windows Audio が再起動した（サービスか audiodg.exe の PID が変わった）、または止まっている: 今の取り込みは音が来なくなっているので、
    // すべて外して作り直す（止まっている間は作らない）。失敗の記録も捨てて、動き出したらすぐ試す。
    // 調べられなかったとき（一時的な失敗）は、前の状態のまま何もしない
    const auto audio = pri::host::GetAudioServiceState();
    bool audioRestarted = false, audioStopped = false;
    if (audio.known)
    {
        const bool serviceChanged = audioService.known && audio.servicePid != audioService.servicePid;
        const bool engineChanged = audioService.known && audio.audiodgPid != 0 && audioService.audiodgPid != 0
                                   && audio.audiodgPid != audioService.audiodgPid;
        audioRestarted = serviceChanged || engineChanged;
        audioStopped = audio.servicePid == 0;
        if (audioRestarted)
            AppLog::write ("app rack \"" + name + "\": Windows Audio " + (audioStopped ? "stopped" : "restarted")
                           + " (service pid " + juce::String (audioService.servicePid) + " -> " + juce::String (audio.servicePid)
                           + ", audiodg pid " + juce::String (audioService.audiodgPid) + " -> " + juce::String (audio.audiodgPid)
                           + "), captures are recreated");
        // audiodg.exe がいない間は、最後にいたときの PID を覚えておく（音を出していないだけのことがあるため）
        audioService = { true, audio.servicePid, audio.audiodgPid != 0 ? audio.audiodgPid : audioService.audiodgPid };
        if (audioStopped)
            audioService.audiodgPid = 0;
    }
    if (audioRestarted)
        failures.clear();

    // 終わったプロセス、対象から外れたプロセス、止まった取り込みを外す。失敗したものは理由を控え、しばらく待ってから試し直す
    for (auto it = sources.begin(); it != sources.end();)
    {
        const auto state = (*it)->capture.GetState();
        if (wanted.count ((*it)->pid) == 0 || state == State::failed || state == State::stopped || audioRestarted || audioStopped)
        {
            if (state == State::failed && ! audioRestarted && ! audioStopped)
            {
                const auto message = (*it)->exe + ": " + juce::String ((*it)->capture.GetLastError().c_str());
                const auto previous = failures.find ((*it)->pid);
                if (previous == failures.end() || previous->second.message != message) // 5 秒ごとの試し直しで同じ失敗を繰り返し書かない
                    AppLog::write ("app rack \"" + name + "\": capture failed, pid " + juce::String ((*it)->pid) + " " + message);
                failures[(*it)->pid] = { message, now + retryDelayMs };
            }
            else if (wanted.count ((*it)->pid) == 0)
            {
                AppLog::write ("app rack \"" + name + "\": stopped capturing " + (*it)->exe + " pid " + juce::String ((*it)->pid)
                               + " (the process ended or was removed from the targets)");
            }

            {
                const juce::SpinLock::ScopedLockType lock (renderLock);
                activeSources.erase (std::remove (activeSources.begin(), activeSources.end(), it->get()), activeSources.end());
            }
            it = sources.erase (it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }

    // 対象から外れたプロセスの失敗の記録は捨てる
    for (auto it = failures.begin(); it != failures.end();)
        it = wanted.count (it->first) == 0 ? failures.erase (it) : std::next (it);

    // 新しく見つかったプロセスを取り込む。準備は取り込みスレッドで行うので、ここでは待たない
    for (const auto& [pid, exe] : wanted)
    {
        if (audioStopped)
            break; // Windows Audio が動き出してから作る

        const bool exists =std::any_of (sources.begin(), sources.end(), [pid = pid] (const auto& source) { return source->pid == pid; });
        if (exists)
            continue;

        if (const auto failure = failures.find (pid); failure != failures.end() && (int) (now - failure->second.retryAtMs) < 0)
            continue; // 失敗したばかり

        if (failures.count (pid) == 0) // 失敗のあとの試し直しは書かない
            AppLog::write ("app rack \"" + name + "\": capturing " + exe + " pid " + juce::String (pid) + " ("
                           + juce::String (preparedRate) + " Hz, " + juce::String (preparedChannels) + " ch)");

        auto source = std::make_shared<Source> (pid, exe, preparedChannels, preparedRate);
        auto* raw = source.get();
        source->capture.Start (pid, preparedRate, preparedChannels,
                               [raw] (const float* data, uint32_t frames, uint32_t channels) { raw->write (data, frames, channels); });
        sources.push_back (std::move (source));
        changed = true;
    }

    // 取り込めていないプロセスがあれば、その理由を出す（取り込めたら消える）
    status.captureError = {};
    for (const auto& [pid, failure] : failures)
    {
        const bool capturing = std::any_of (sources.begin(), sources.end(), [pid = pid] (const auto& source)
                                            { return source->pid == pid && source->capture.GetState() == State::running; });
        if (! capturing)
        {
            status.captureError = failure.message;
            break;
        }
    }

    if (changed)
        publishSources();

    currentRoots.clear();
    for (const auto& source : sources)
        currentRoots.push_back (source->pid);
    status.processCount = currentRoots.empty() ? 0 : (int) pri::host::CollectProcessTree (currentRoots).size();
}

// 対象アプリがどのデバイスに音を出しているかを調べる
void AppRack::updateOutputs()
{
    status.activeOutputs.clear();
    status.doubled = false;

    const auto now = juce::Time::getMillisecondCounter();

    if (! currentRoots.empty())
    {
        const auto tree = pri::host::CollectProcessTree (currentRoots);

        // 実際に音が出ているセッションのデバイスを覚える（無音で開いているだけのセッションは数えない。
        // Gather などは既定のデバイスに無音のまま開いているので、それを「二重に聞こえる」としないため）
        for (const auto& session : pri::host::ListAudioSessions())
            if (session.sounding && tree.count (session.processId) != 0)
                heardOutputs[juce::String (session.endpointGuid.c_str()).toUpperCase()] = { juce::String (session.endpointName.c_str()), now };
    }
    else
    {
        heardOutputs.clear();
    }

    // 最近（10 秒以内）音が出ていたデバイスを出力先とする（会話の切れ目で表示がちらつかないように）
    const auto mainGuid = (mainEndpointGuid != nullptr ? mainEndpointGuid() : juce::String()).toUpperCase();
    for (auto it = heardOutputs.begin(); it != heardOutputs.end();)
    {
        if (now - it->second.lastHeardMs > heardMemoryMs)
        {
            it = heardOutputs.erase (it);
            continue;
        }
        status.activeOutputs.addIfNotAlreadyThere (it->second.name);
        if (mainGuid.isNotEmpty() && it->first == mainGuid)
            status.doubled = true;
        ++it;
    }

    // 合流までの遅れ: FIFO に溜まっている量 + 取り込みの塊（約 10 ms）
    int ready = 0;
    for (const auto& source : sources)
        ready = juce::jmax (ready, source->lastReady.load());
    status.delayMs = sources.empty() ? 0.0 : 1000.0 * ready / (double) preparedRate + 10.0;
}

//==============================================================================
void AppRack::render (float* const* out, uint32_t channels, uint32_t frames, uint32_t sampleRate)
{
    for (uint32_t ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::clear (out[ch], (int) frames);

    // 準備し直している最中なら、この周期は無音にする（音声スレッドを待たせない）
    const juce::SpinLock::ScopedTryLockType lock (renderLock);
    if (! lock.isLocked() || sampleRate != preparedRate || channels != preparedChannels || frames > preparedMaxFrames
        || mixBuffer.getNumChannels() < (int) channels || mixBuffer.getNumSamples() < (int) frames)
        return;

    if (sampleRate != smoothingRate)
    {
        smoothingRate = sampleRate;
        inputGain.reset ((double) sampleRate, 0.02);
        outputGain.reset ((double) sampleRate, 0.02);
        inputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGainDb.load()));
    }
    inputGain.setTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load (std::memory_order_relaxed)));
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputGainDb.load (std::memory_order_relaxed)));

    auto* const* mix = mixBuffer.getArrayOfWritePointers();
    for (uint32_t ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::clear (mix[ch], (int) frames);

    for (auto* source : activeSources)
        source->addTo (mix, frames, preparedMaxFrames, sampleRate, readBuffer, sourceBuffer);

    // 入力ゲイン → プラグイン → 合流ゲイン
    inputMeter.beginBlock (sampleRate, channels);
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const float gain = inputGain.getNextValue();
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            mix[ch][frame] *= gain;
            inputMeter.add (ch, mix[ch][frame]);
        }
    }

    chain.process (mix, channels, frames, sampleRate);

    outputMeter.beginBlock (sampleRate, channels);
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const float gain = outputGain.getNextValue();
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            const float y = mix[ch][frame] * gain;
            outputMeter.add (ch, y);
            out[ch][frame] = y;
        }
    }

    inputMeter.endBlock();
    outputMeter.endBlock();
}

//==============================================================================
void AppRack::saveState (juce::ValueTree& parent) const
{
    juce::ValueTree tree ("AppRack");
    tree.setProperty ("name", name, nullptr);
    tree.setProperty ("targets", targets.joinIntoString ("|"), nullptr);
    tree.setProperty ("inputGainDb", inputGainDb.load(), nullptr);
    tree.setProperty ("outputGainDb", outputGainDb.load(), nullptr);
    chain.saveState (tree);
    parent.appendChild (tree, nullptr);
}

void AppRack::restoreState (const juce::ValueTree& rackTree, juce::StringArray& errors, bool loadLater)
{
    name = rackTree.getProperty ("name", name).toString();
    // 以前の "merge"（合流先。0 = 入力の前）は読まない。合流先は出力の手前に固定
    inputGainDb = (float) rackTree.getProperty ("inputGainDb", 0.0f);
    outputGainDb = (float) rackTree.getProperty ("outputGainDb", 0.0f);
    chain.restoreState (rackTree, errors, loadLater);

    juce::StringArray restored;
    restored.addTokens (rackTree.getProperty ("targets").toString(), "|", "");
    setTargets (restored);
}
