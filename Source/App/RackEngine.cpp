#include "RackEngine.h"

#include <algorithm>
#include <map>
#include <set>

namespace
{
// ノードを線でつないでいた頃の形式（version 1）を、プラグインの並び（Plugin の子）に読み替える。
// 入力（id 1）から線をたどった順に並べ、線でつながっていなかったプラグインは末尾に OFF で入れる
juce::ValueTree convertVersion1 (const juce::ValueTree& state)
{
    std::map<int, juce::ValueTree> nodes;
    for (const auto& child : state)
        if (child.hasType ("Node"))
            nodes[(int) child.getProperty ("id")] = child;

    juce::ValueTree converted ("PRISystemRack");
    auto addPlugin = [&converted] (const juce::ValueTree& node, bool enabled)
    {
        juce::ValueTree plugin ("Plugin");
        plugin.setProperty ("enabled", enabled, nullptr);
        plugin.setProperty ("file", node.getProperty ("file"), nullptr);
        plugin.setProperty ("description", node.getProperty ("description"), nullptr);
        plugin.setProperty ("state", node.getProperty ("state"), nullptr);
        converted.appendChild (plugin, nullptr);
    };

    std::set<int> used;
    int next = nodes.count (1) != 0 ? (int) nodes[1].getProperty ("next") : 0;
    while (next != 0 && nodes.count (next) != 0 && used.insert (next).second)
    {
        const auto& node = nodes[next];
        if ((int) node.getProperty ("type") == 2)
            addPlugin (node, ! (bool) node.getProperty ("bypassed", false));
        next = node.getProperty ("next");
    }

    for (const auto& [id, node] : nodes)
        if ((int) node.getProperty ("type") == 2 && used.count (id) == 0)
            addPlugin (node, false);

    return converted;
}
} // namespace

RackEngine::RackEngine()
{
    chain.addChangeListener (this);
    applyFormat (formatRate, formatChannels, formatMaxFrames);
}

RackEngine::~RackEngine()
{
    stopTimer(); // 読み込みの途中で終了したとき
    link.reset(); // 音声スレッドを止めてから片付ける
    silentStream.Stop();
    cancelPendingUpdate();
    chain.removeChangeListener (this);

    {
        const juce::SpinLock::ScopedLockType lock (processLock);
        activeRacks.clear();
    }

    for (auto& rack : appRacks)
        rack->removeChangeListener (this);
    appRacks.clear();
}

void RackEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    sendChangeMessage(); // チェーンやアプリ用ラックの変更も、ラック全体の変更として知らせる
}

//==============================================================================
void RackEngine::setEndpoint (const juce::String& friendlyName, const juce::String& guid)
{
    if (guid == endpointGuid && (link != nullptr || guid.isEmpty()))
        return;

    endpointName = friendlyName;
    endpointGuid = guid;
    startLink();

    updateSilentStream();
    sendChangeMessage();
}

void RackEngine::setProcessingBuffer (int milliseconds, bool autoAdjust)
{
    milliseconds = juce::jlimit (0, ProcessingBuffer::maxMilliseconds, milliseconds);
    const bool wasOn = processingBufferAuto || processingBufferMs > 0;
    const bool on = autoAdjust || milliseconds > 0;

    processingBufferMs = milliseconds;
    processingBufferAuto = autoAdjust;

    if (processingBuffer != nullptr)
    {
        processingBuffer->setTargetMilliseconds (milliseconds);
        processingBuffer->setAutoAdjust (autoAdjust);
    }

    // オフとオンが切り替わるときだけ、処理バッファを作り直して APO とつなぎ直す
    if (on != wasOn && endpointGuid.isNotEmpty())
        startLink();
}

void RackEngine::startLink()
{
    // 先に APO とのやり取りを止めてから処理バッファを消す（やり取りのスレッドが処理バッファを使っているため）
    link.reset();
    processingBuffer.reset();

    if (endpointGuid.isEmpty())
        return;

    pri::host::ApoLink::Processor* processor = this;
    if (processingBufferAuto || processingBufferMs > 0)
    {
        processingBuffer = std::make_unique<ProcessingBuffer> (*this, processingBufferMs, processingBufferAuto);
        processor = processingBuffer.get();
    }

    link = std::make_unique<pri::host::ApoLink> (std::wstring (endpointGuid.toWideCharPointer()), *processor);
    link->Start();
}

pri::host::ApoLink::Stats RackEngine::getLinkStats() const
{
    return link != nullptr ? link->GetStats() : pri::host::ApoLink::Stats {};
}

juce::String RackEngine::describeFormat() const
{
    return juce::String (formatRate) + " Hz / " + juce::String (formatChannels) + "ch";
}

// メイン画面の入力・出力の欄には説明を出さない（入力は常に Windows がこのデバイスで鳴らす音、出力は常にこのデバイスで、
// デバイス名は上の欄の「再生デバイス」に出ているため。見出しの行をメーターの種類の名前と最終的なレイテンシーに空ける）
juce::String RackEngine::getInputSubtitle() const
{
    return {};
}

juce::String RackEngine::getOutputSubtitle() const
{
    return {};
}

juce::String RackEngine::describeTotalLatency() const
{
    // プラグインの合計 + 処理バッファ
    return TRANS ("Total latency: %latency%")
        .replace ("%latency%", PluginChain::formatLatency (chain.getTotalLatencySamples() + getProcessingBufferSamples(),
                                                         (double) chain.getSampleRate()));
}

std::vector<RackModel::MergedRack> RackEngine::getMergedRacks() const
{
    std::vector<MergedRack> racks;
    for (const auto& rack : appRacks)
        racks.push_back ({ rack->getName(), rack->getChain().isBypassAll() });
    return racks;
}

//==============================================================================
AppRack* RackEngine::findAppRack (int id) const
{
    for (const auto& rack : appRacks)
        if (rack->getId() == id)
            return rack.get();
    return nullptr;
}

AppRack* RackEngine::addAppRack()
{
    if (! appRacksEnabled || (int) appRacks.size() >= maxAppRacks)
        return nullptr;

    auto rack = std::make_unique<AppRack> (nextRackId++, [this] { return endpointGuid; },
                                           [this] { return getProcessingBufferSamples(); });
    rack->prepare (formatRate, formatChannels, formatMaxFrames);
    rack->addChangeListener (this);
    auto* added = rack.get();
    appRacks.push_back (std::move (rack));

    publishRacks();
    updateSilentStream();
    sendChangeMessage();
    return added;
}

void RackEngine::removeAppRack (int id)
{
    for (auto it = appRacks.begin(); it != appRacks.end(); ++it)
    {
        if ((*it)->getId() != id)
            continue;

        auto removed = std::move (*it);
        appRacks.erase (it);
        publishRacks(); // ここで音声スレッドの並びからも外れる
        removed->removeChangeListener (this);
        removed.reset();

        updateSilentStream();
        sendChangeMessage();
        return;
    }
}

void RackEngine::setAppRacksEnabled (bool shouldBeEnabled)
{
    if (shouldBeEnabled == appRacksEnabled)
        return;

    appRacksEnabled = shouldBeEnabled;

    if (! shouldBeEnabled)
    {
        // 今のラックを止めて、中身を預かる（読み込み途中のプラグインは、控えてある状態が書かれる）
        juce::ValueTree saved ("Racks");
        for (const auto& rack : appRacks)
            rack->saveState (saved);
        clearAppRacks();
        for (const auto& child : saved)
            heldAppRacks.push_back (child.createCopy());
    }
    else if (! heldAppRacks.empty())
    {
        // 預かっていた中身からラックを作り、プラグインを 1 つずつ読み込む（読み込み中なら、その続きで読み込まれる）
        for (const auto& tree : std::exchange (heldAppRacks, {}))
            buildAppRack (tree);
        publishRacks();

        if (loadingPlugins)
        {
            for (const auto& rack : appRacks) // 無効の間はラックがなかったので、どれも今作ったもの
                loadTotal += rack->getChain().countPendingLoads();
        }
        else
        {
            startLoadingPlugins();
        }
    }

    updateSilentStream();
    sendChangeMessage();
}

void RackEngine::buildAppRack (const juce::ValueTree& rackTree)
{
    auto rack = std::make_unique<AppRack> (nextRackId++, [this] { return endpointGuid; },
                                           [this] { return getProcessingBufferSamples(); });
    rack->prepare (formatRate, formatChannels, formatMaxFrames);
    rack->getChain().setSuspended (true); // 読み込み終わるまで一時停止（音は素通し）
    rack->restoreState (rackTree, loadErrors, true);
    rack->addChangeListener (this);
    appRacks.push_back (std::move (rack));
}

void RackEngine::clearAppRacks()
{
    {
        const juce::SpinLock::ScopedLockType lock (processLock);
        activeRacks.clear();
    }
    for (auto& rack : appRacks)
        rack->removeChangeListener (this);
    appRacks.clear();
}

void RackEngine::publishRacks()
{
    std::vector<AppRack*> list;
    for (const auto& rack : appRacks)
        list.push_back (rack.get());

    const juce::SpinLock::ScopedLockType lock (processLock);
    activeRacks.swap (list);
}

void RackEngine::setTestToneOn (bool output, bool shouldBeOn)
{
    (output ? outputTone : inputTone).setOn (shouldBeOn);
    updateSilentStream();
}

// アプリ用ラックがあるときと音声テストを鳴らしているときは、ほかに何も鳴っていなくても APO が動くよう無音を流す
void RackEngine::updateSilentStream()
{
    const bool needed = ! appRacks.empty() || inputTone.isOn() || outputTone.isOn();
    if (needed && endpointGuid.isNotEmpty())
        silentStream.Start (std::wstring (endpointGuid.toWideCharPointer()));
    else
        silentStream.Stop();
}

//==============================================================================
juce::ValueTree RackEngine::saveState() const
{
    juce::ValueTree state ("PRISystemRack");
    state.setProperty ("version", 3, nullptr);
    state.setProperty ("inputGainDb", inputGainDb.load(), nullptr);
    state.setProperty ("outputGainDb", outputGainDb.load(), nullptr);
    chain.saveState (state);

    for (const auto& rack : appRacks)
        rack->saveState (state);
    // 機能が無効のあいだ預かっているラック（そのまま書き戻す）。ただしセーフモードの印（deferLoad）は外す。
    // 作ったラックの行は保存のときにこの印を書かないので、それと同じにする（残すと、あとで有効にしても読み込まれないままになる）
    for (const auto& held : heldAppRacks)
    {
        auto copy = held.createCopy();
        for (auto child : copy)
            if (child.hasType ("Plugin"))
                child.removeProperty ("deferLoad", nullptr);
        state.appendChild (copy, nullptr);
    }

    return state;
}

void RackEngine::restoreState (const juce::ValueTree& state)
{
    if (! state.hasType ("PRISystemRack"))
        return;

    // 前の読み込みが途中なら、やめて始め直す
    stopTimer();
    loadErrors.clear();
    juce::StringArray& errors = loadErrors;

    inputGainDb = (float) state.getProperty ("inputGainDb", 0.0f);
    outputGainDb = (float) state.getProperty ("outputGainDb", 0.0f);

    // 行だけ戻す（プラグインはあとで 1 つずつ読み込む）。読み込み終わるまで並びは一時停止（音は素通し）
    chain.setSuspended (true);
    const int version = state.getProperty ("version", 1);
    if (version >= 2)
    {
        chain.restoreState (state, errors, true);
    }
    else
    {
        auto converted = convertVersion1 (state);
        converted.setProperty ("bypassAll", state.getProperty ("bypassAll", false), nullptr);
        chain.restoreState (converted, errors, true);
    }

    // アプリ用ラック（機能が無効なら作らずに中身を預かる）
    clearAppRacks();
    heldAppRacks.clear();
    for (const auto& child : state)
    {
        if (! child.hasType ("AppRack"))
            continue;
        if (appRacksEnabled)
            buildAppRack (child);
        else
            heldAppRacks.push_back (child.createCopy());
    }

    publishRacks();
    updateSilentStream();
    startLoadingPlugins();
}

void RackEngine::startLoadingPlugins()
{
    // プラグインを 1 つずつ読み込み始める（読み込むものがなければすぐ終わる）
    loadingPlugins = true;
    loadedCount = 0;
    loadTotal = 0;
    for (auto* c : allChains())
        loadTotal += c->countPendingLoads();
    lastLoadEndMs = juce::Time::getMillisecondCounter(); // 最初の 1 つも、画面を描いてから始める
    sendChangeMessage();
    startTimer (15);
}

//==============================================================================
const RackEngine::Overload& RackEngine::checkOverload()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (overloadPrimed && now - overloadCheckedMs < 400.0)
        return overload;
    overloadCheckedMs = now;

    // 処理の負荷（前に調べてからの分）
    const auto busy = busyTicks.load (std::memory_order_relaxed);
    const auto frames = processedFrames.load (std::memory_order_relaxed);
    const auto rate = linkRate.load (std::memory_order_relaxed);
    overload.load = -1.0f;
    if (overloadPrimed && frames > lastProcessedFrames && rate > 0)
        overload.load = (float) (juce::Time::highResolutionTicksToSeconds (busy - lastBusyTicks)
                                 / ((double) (frames - lastProcessedFrames) / rate));
    lastBusyTicks = busy;
    lastProcessedFrames = frames;

    // バッファ不足・時間切れが増えたか（バッファを作り直すと回数は 0 に戻るので、減ったら数え直す）
    const auto underruns = getBufferUnderruns();
    const auto timeouts = (juce::uint64) getLinkStats().apoTimedOut;
    const bool dropped = overloadPrimed && (underruns > lastUnderruns || timeouts > lastTimeouts);
    lastUnderruns = underruns;
    lastTimeouts = timeouts;
    overloadPrimed = true;

    dropHistory = ((dropHistory << 1) | (dropped ? 1u : 0u)) & 0xfu;
    if (juce::countNumberOfBits ((juce::uint32) dropHistory) >= 2)
        lastOverloadMs = now;
    overload.active = lastOverloadMs > 0.0 && now - lastOverloadMs < 3000.0;
    if (now < previewOverloadUntilMs) // 確認用（--preview-overload）
    {
        overload.active = true;
        overload.load = 1.35f;
    }

    overload.buffered = processingBuffer != nullptr;
    overload.bufferAuto = processingBufferAuto;
    overload.bufferAtMax = processingBuffer != nullptr
                        && processingBuffer->getCurrentMilliseconds() >= ProcessingBuffer::maxMilliseconds - 0.5;

    // 重いプラグイン（行の「処理の重さ」と同じ値。1 周期の処理にかかった時間 ÷ 周期の長さの平均）
    overload.heavyPlugins.clear();
    if (overload.active)
    {
        std::vector<std::pair<float, juce::String>> loads;
        for (auto* c : allChains())
            for (const auto& plugin : c->getPlugins())
                if (plugin->enabled.load() && plugin->instance != nullptr && ! plugin->faulted.load() && plugin->tap.isFresh())
                    loads.emplace_back (plugin->tap.getLoadAverage(), plugin->getDisplayName());

        std::sort (loads.begin(), loads.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [load, name] : loads)
            if (load >= 0.05f && overload.heavyPlugins.size() < 3)
                overload.heavyPlugins.add (name + " " + juce::String (juce::roundToInt (load * 100.0f)) + "%");
    }
    return overload;
}

juce::String RackEngine::Overload::describeTitle() const
{
    return buffered ? TRANS ("Processing cannot keep up, so the sound is cutting out or silent")
                    : TRANS ("Processing cannot keep up, so some sound is not going through the plugins");
}

juce::String RackEngine::Overload::describeDetail() const
{
    juce::StringArray lines;

    juce::String figures;
    if (load >= 0.0f)
        figures = TRANS ("Processing load %load%").replace ("%load%", juce::String (juce::roundToInt (load * 100.0f)) + "%");
    if (! heavyPlugins.isEmpty())
        figures << (figures.isEmpty() ? "" : "  /  ")
                << TRANS ("Heavy plugins: %list%").replace ("%list%", heavyPlugins.joinIntoString (", "));
    if (figures.isNotEmpty())
        lines.add (figures);

    if (load >= 1.0f || (buffered && (bufferAuto || bufferAtMax)))
        lines.add (TRANS ("The PC cannot process this in real time. Turn off some plugins, or stop other heavy work on the PC."));
    else if (! buffered)
        lines.add (TRANS ("Processing is sometimes too slow. Turning on the buffer may help."));
    else
        lines.add (TRANS ("Processing is sometimes too slow. Increasing the buffer may help."));

    return lines.joinIntoString ("\n");
}

std::vector<PluginChain*> RackEngine::allChains()
{
    std::vector<PluginChain*> chains { &chain };
    for (const auto& rack : appRacks)
        chains.push_back (&rack->getChain());
    return chains;
}

void RackEngine::timerCallback()
{
    // プラグインが初期化の中でメッセージを回すと、ここに入れ子で入ることがある。そのときは何もしない。
    // ほかの経路（足す・置き換えるなど）でプラグインを作っている途中も、作り始めると失敗するので終わるまで待つ
    if (loadingOne || PluginChain::isCreatingInstance())
        return;

    // 1 つ読み込んだら、次まで実際の時間で loadGapMs 空ける。その間はすぐ戻り、画面の描き直し（Direct2D は画面の更新のタイミングごとに
    // 届く知らせで描く）を先に済ませる。タイマーの間隔だけで空けようとしても、長い読み込みのあとは次がすぐ来て描き直しが後回しになる
    constexpr juce::uint32 loadGapMs = 80;
    if (juce::Time::getMillisecondCounter() - lastLoadEndMs < loadGapMs)
        return;

    {
        const juce::ScopedValueSetter<bool> busy (loadingOne, true);
        for (auto* c : allChains())
        {
            if (c->loadNextPending (loadErrors))
            {
                ++loadedCount;
                lastLoadEndMs = juce::Time::getMillisecondCounter();
                sendChangeMessage(); // 行の表示を変える（読み込み中 → いつもの表示）と、進み具合
                return;
            }
        }
    }

    stopTimer();
    finishLoadingPlugins();
}

juce::String RackEngine::getNextLoadingName()
{
    for (auto* c : allChains())
        for (const auto& plugin : c->getPlugins())
            if (plugin->pendingLoad)
                return plugin->description.name;
    return {};
}

void RackEngine::finishLoadingPlugins()
{
    // 全部そろったので、一度に切り替える
    for (auto* c : allChains())
        c->setSuspended (false);
    loadingPlugins = false;
    sendChangeMessage();

    if (onPluginsLoaded != nullptr)
        onPluginsLoaded();
}

//==============================================================================
void RackEngine::Prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames)
{
    linkRate = sampleRate;
    linkChannels = channels;
    linkMaxFrames = maxFrames;
    triggerAsyncUpdate(); // プラグインの準備はメッセージスレッドで行う
}

void RackEngine::handleAsyncUpdate()
{
    applyFormat (linkRate.load(), linkChannels.load(), linkMaxFrames.load());
}

void RackEngine::applyFormat (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames)
{
    if (sampleRate == 0 || channels == 0 || maxFrames == 0)
        return;

    formatRate = sampleRate;
    formatChannels = channels;
    formatMaxFrames = maxFrames;

    chain.prepare (sampleRate, channels, maxFrames);
    for (const auto& rack : appRacks)
        rack->prepare (sampleRate, channels, maxFrames);

    if (processRate == sampleRate && processChannels == channels && processMaxFrames == maxFrames)
        return;

    // 確保はロックの外で行う
    juce::AudioBuffer<float> main ((int) channels, (int) maxFrames), racks ((int) channels, (int) maxFrames);
    main.clear();
    racks.clear();

    const juce::SpinLock::ScopedLockType lock (processLock);
    std::swap (mainBuffer, main);
    std::swap (rackBuffer, racks);
    processRate = sampleRate;
    processChannels = channels;
    processMaxFrames = maxFrames;
}

void RackEngine::Process (const float* input, float* output, uint32_t frames, uint32_t channels)
{
    juce::ScopedNoDenormals noDenormals;

    // 処理の負荷（checkOverload）: この呼び出しにかかった時間と音の長さを足す（ほかのプロセスに CPU を取られて待った時間も含む）
    struct LoadScope
    {
        RackEngine& engine;
        uint32_t frames;
        juce::int64 start = juce::Time::getHighResolutionTicks();
        ~LoadScope()
        {
            engine.busyTicks.fetch_add (juce::Time::getHighResolutionTicks() - start, std::memory_order_relaxed);
            engine.processedFrames.fetch_add (frames, std::memory_order_relaxed);
        }
    } loadScope { *this, frames };

    const auto rate = linkRate.load (std::memory_order_relaxed);
    if (rate != smoothingRate && rate > 0)
    {
        smoothingRate = rate;
        inputGain.reset ((double) rate, 0.02);
        outputGain.reset ((double) rate, 0.02);
        inputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load()));
        outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGainDb.load()));
    }
    inputGain.setTargetValue (juce::Decibels::decibelsToGain (inputGainDb.load (std::memory_order_relaxed)));
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputGainDb.load (std::memory_order_relaxed)));

    // 入力・出力の欄のメーター（ピーク・RMS・ラウドネスの元の値）
    inputMeter.beginBlock (rate, channels);
    outputMeter.beginBlock (rate, channels);

    const juce::SpinLock::ScopedLockType lock (processLock);

    const bool usable = rate == processRate && channels == processChannels && frames <= processMaxFrames
                     && mainBuffer.getNumChannels() >= (int) channels && mainBuffer.getNumSamples() >= (int) frames;

    if (! usable)
    {
        // 準備ができるまではゲインだけ
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            const float gainIn = inputGain.getNextValue();
            const float gainOut = outputGain.getNextValue();

            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                const size_t i = (size_t) frame * channels + ch;
                const float x = input[i] * gainIn;
                const float y = x * gainOut;
                inputMeter.add (ch, x);
                outputMeter.add (ch, y);
                output[i] = y;
            }
        }

        inputMeter.endBlock();
        outputMeter.endBlock();
        outputGuard.process (output, frames, channels, rate); // 出力の保護（最後にかける）
        return;
    }

    auto* const* main = mainBuffer.getArrayOfWritePointers();
    auto* const* rackOut = rackBuffer.getArrayOfWritePointers();

    // インターリーブを分ける
    for (uint32_t frame = 0; frame < frames; ++frame)
        for (uint32_t ch = 0; ch < channels; ++ch)
            main[ch][frame] = input[(size_t) frame * channels + ch];

    inputTone.addTo (main, channels, frames, rate); // 音声テスト（入力）: 入力ゲインの手前

    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const float gainIn = inputGain.getNextValue();
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            main[ch][frame] *= gainIn;
            inputMeter.add (ch, main[ch][frame]);
        }
    }

    chain.process (main, channels, frames, rate);

    // アプリ用ラックの音を足す（メインのプラグインのあと、出力ゲインの手前）
    for (auto* rack : activeRacks)
    {
        rack->render (rackOut, channels, frames, rate);
        for (uint32_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::add (main[ch], rackOut[ch], (int) frames);
    }
    outputTone.addTo (main, channels, frames, rate); // 音声テスト（出力）: プラグインのあと、出力ゲインの手前

    // 出力ゲインをかけながらインターリーブに戻す
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const float gainOut = outputGain.getNextValue();
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            const float y = main[ch][frame] * gainOut;
            outputMeter.add (ch, y);
            output[(size_t) frame * channels + ch] = y;
        }
    }

    inputMeter.endBlock();
    outputMeter.endBlock();
    outputGuard.process (output, frames, channels, rate); // 出力の保護（NaN・無限大を 0 に、-0.3 dBFS を超えないように。最後にかける）
}
