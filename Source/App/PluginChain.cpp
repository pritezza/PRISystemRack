#include "PluginChain.h"

#include "AppLog.h"
#include "PluginGuard.h"
#include "PluginSafety.h"

namespace
{
// メインバスを指定のチャンネル構成にする
bool setMainBuses (juce::AudioPluginInstance& plugin, const juce::AudioChannelSet& set)
{
    auto layout = plugin.getBusesLayout();

    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference (0) = set;

    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = set;

    return plugin.setBusesLayout (layout);
}

int pluginChannelCount (const juce::AudioPluginInstance& plugin)
{
    return juce::jmax (plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
}

// 無音とみなす大きさ（-120 dB 未満）。完全な 0 でなく、アプリが流すごく小さなノイズやディザーも無音とみなす
constexpr float silenceThreshold = 1.0e-6f;

bool isSilent (float* const* channels, uint32_t numChannels, uint32_t frames)
{
    for (uint32_t ch = 0; ch < numChannels; ++ch)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (channels[ch], (int) frames);
        if (std::abs (range.getStart()) >= silenceThreshold || std::abs (range.getEnd()) >= silenceThreshold)
            return false;
    }
    return true;
}

// 控えてある状態をプラグインに戻す（空なら何もしない）。途中で例外が起きたら false（そのインスタンスはもう使わない）
bool applyState (juce::AudioPluginInstance& instance, const juce::MemoryBlock& state)
{
    return state.getSize() == 0 || PluginGuard::setState (&instance, state.getData(), (int) state.getSize());
}
} // namespace

void PluginChain::Plugin::setSnapshot (const juce::Image& captured)
{
    if (! captured.isValid())
        return;

    // 保存する大きさ（行の表示と、マウスを乗せたときの拡大に使う）。設定ファイルとプリセットに入るので小さめの JPEG にする
    constexpr int maxWidth = 560;
    constexpr int maxHeight = 420;
    const double scale = juce::jmin (1.0, (double) maxWidth / captured.getWidth(), (double) maxHeight / captured.getHeight());
    const auto image = scale < 1.0 ? captured.rescaled (juce::jmax (1, juce::roundToInt (captured.getWidth() * scale)),
                                                        juce::jmax (1, juce::roundToInt (captured.getHeight() * scale)),
                                                        juce::Graphics::highResamplingQuality)
                                   : captured;

    juce::MemoryOutputStream stream;
    juce::JPEGImageFormat format;
    format.setQuality (0.8f);
    if (! format.writeImageToStream (image, stream))
        return;

    snapshot = image;
    snapshotJpeg = stream.getMemoryBlock();
    ++snapshotVersion;
}

void PluginChain::Plugin::setSnapshotJpeg (const juce::MemoryBlock& jpeg)
{
    snapshotJpeg = jpeg;
    snapshot = jpeg.isEmpty() ? juce::Image() : juce::ImageFileFormat::loadFrom (jpeg.getData(), jpeg.getSize());
    if (! snapshot.isValid())
        snapshotJpeg.reset();
    ++snapshotVersion;
}

juce::String PluginChain::Plugin::getDisplayName() const
{
    if (instance != nullptr)
        return instance->getName();
    if (description.name.isNotEmpty())
        return description.name;
    return juce::File (description.fileOrIdentifier).getFileNameWithoutExtension();
}

PluginChain::PluginChain()
{
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());
    midiBuffer.ensureSize (2048);
    publish();
}

PluginChain::~PluginChain()
{
    cancelPendingUpdate();

    {
        const juce::SpinLock::ScopedLockType lock (chainLock);
        chain.clear();
    }

    for (auto& plugin : plugins)
        disposeInstance (std::move (plugin->instance), plugin->faulted.load());

    plugins.clear();
}

void PluginChain::disposeInstance (std::unique_ptr<juce::AudioPluginInstance> instance, bool faulted)
{
    if (instance == nullptr)
        return;

    // 例外で途中から抜けたプラグインは、JUCE のホスト側（VST3PluginInstance）のスピンロックが掛かったまま残る。
    // 解放すると releaseResources がそのロックを待ち続けて止まる（GuardTest で確認）ので、解放せずに手放す
    if (faulted)
    {
        instance.release();
        return;
    }

    PluginGuard::destroy (instance.release());
}

void PluginChain::handleAsyncUpdate()
{
    // 音声スレッドでプラグインが例外を起こした
    if (faultPending.exchange (false))
    {
        // 新しくエラーで止まったものを app.log に書く（音声スレッドでは書けないのでここで）
        for (const auto& plugin : plugins)
            if (plugin->faulted.load() && ! plugin->faultLogged)
            {
                plugin->faultLogged = true;
                logFault (*plugin, "processBlock");
            }
        sendChangeMessage();
    }
}

//==============================================================================
PluginChain::Plugin* PluginChain::findPlugin (int id) const
{
    for (const auto& plugin : plugins)
        if (plugin->id == id)
            return plugin.get();

    return nullptr;
}

int PluginChain::indexOf (int id) const
{
    for (size_t i = 0; i < plugins.size(); ++i)
        if (plugins[i]->id == id)
            return (int) i;

    return -1;
}

int PluginChain::countEnabled() const
{
    int count = 0;
    for (const auto& plugin : plugins)
        if (plugin->enabled.load())
            ++count;

    return count;
}

std::unique_ptr<PluginChain::Plugin> PluginChain::makePlugin (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                              const juce::PluginDescription& description, bool enabled)
{
    auto plugin = std::make_unique<Plugin>();
    plugin->id = nextPluginId++;
    plugin->instance = std::move (instance);
    plugin->description = description;
    plugin->enabled = enabled;
    preparePlugin (*plugin);
    return plugin;
}

juce::String PluginChain::describeFull()
{
    return TRANS ("A rack can hold up to %count% plugins. Remove one to add another.").replace ("%count%", juce::String (maxPlugins));
}

PluginChain::Plugin* PluginChain::addPlugin (const juce::PluginDescription& wanted, juce::String& errorMessage, int index)
{
    if (isFull())
    {
        errorMessage = describeFull();
        return nullptr;
    }

    juce::PluginDescription description;
    auto instance = createInstance (wanted, description, errorMessage);

    if (instance == nullptr)
        return nullptr;

    auto plugin = makePlugin (std::move (instance), description, true);
    plugin->view = defaultView;
    auto* added = plugin.get();

    if (index < 0 || index >= (int) plugins.size())
        plugins.push_back (std::move (plugin));
    else
        plugins.insert (plugins.begin() + index, std::move (plugin));

    publish();
    return added;
}

PluginChain::Plugin* PluginChain::duplicatePlugin (int id, juce::String& errorMessage)
{
    const int index = indexOf (id);

    if (index < 0)
        return nullptr;

    if (isFull())
    {
        errorMessage = describeFull();
        return nullptr;
    }

    const auto& source = *plugins[(size_t) index];

    juce::MemoryBlock state;
    if (source.instance == nullptr || source.faulted.load() || ! PluginGuard::getState (source.instance.get(), &state))
    {
        errorMessage = TRANS ("This plugin cannot be duplicated because it is not loaded or has stopped after an error. Reload it first.");
        return nullptr;
    }

    juce::PluginDescription description;
    auto instance = createInstance (source.description, description, errorMessage);

    if (instance == nullptr)
        return nullptr;

    if (! applyState (*instance, state))
    {
        disposeInstance (std::move (instance), true);
        errorMessage = TRANS ("The plugin crashed while restoring its settings.");
        return nullptr;
    }

    auto plugin = makePlugin (std::move (instance), description, source.enabled.load());
    plugin->alwaysProcess = source.alwaysProcess.load();
    plugin->view = source.view;
    plugin->setSnapshotJpeg (source.snapshotJpeg); // 同じ設定なので、エディターの画面も同じ
    if (source.editorBounds.has_value())
        plugin->editorBounds = source.editorBounds->translated (30, 30); // 元の画面と重ならないよう少しずらす
    auto* added = plugin.get();
    plugins.insert (plugins.begin() + index + 1, std::move (plugin));
    publish();
    return added;
}

void PluginChain::removePlugin (int id)
{
    const int index = indexOf (id);

    if (index < 0)
        return;

    auto removed = std::move (plugins[(size_t) index]);
    plugins.erase (plugins.begin() + index);
    publish(); // ここで音声スレッドの並びからも外れる
    disposeInstance (std::move (removed->instance), removed->faulted.load()); // プラグインはメッセージスレッドで解放する
}

void PluginChain::releaseInstance (Plugin& plugin)
{
    // 先に音声スレッドの並びから外す（外す前に instance を抜くと、処理中の音声スレッドが nullptr を使う）
    {
        const juce::SpinLock::ScopedLockType lock (chainLock);
        chain.erase (std::remove (chain.begin(), chain.end(), &plugin), chain.end());
    }

    auto instance = std::move (plugin.instance);
    publish(); // instance が nullptr の行は音声スレッドの並びに入らない
    disposeInstance (std::move (instance), plugin.faulted.load());
}

bool PluginChain::reloadPlugin (int id, juce::String& errorMessage)
{
    auto* plugin = findPlugin (id);

    if (plugin == nullptr)
        return false;

    // 今の状態を控える（取れなければ前に控えた状態を使う）。例外を起こしたものには触らない
    if (plugin->instance != nullptr && ! plugin->faulted.load())
    {
        juce::MemoryBlock state;
        if (PluginGuard::getState (plugin->instance.get(), &state))
            plugin->savedState = std::move (state);
    }

    // 先に解放して DLL を手放してから読み込み直す（作り直した DLL も読み込まれるように）。
    // 同じプラグインを読み込む（WaveShell のように 1 つのファイルに複数入っていても、先頭のものに変わらないように）
    releaseInstance (*plugin);
    plugin->faulted = false;
    plugin->faultLogged = false; // 読み込み直したものがまたエラーで止まったら、もう一度 app.log に書く

    juce::PluginDescription description;
    auto instance = createInstance (plugin->description, description, errorMessage);

    if (instance == nullptr)
    {
        plugin->loadError = errorMessage;
        sendChangeMessage();
        return false;
    }

    if (! applyState (*instance, plugin->savedState))
    {
        // 状態を戻す途中で例外が起きた。そのインスタンスは使わずに手放し、状態は控えたままにする
        disposeInstance (std::move (instance), true);
        errorMessage = TRANS ("The plugin crashed while restoring its settings.");
        plugin->loadError = errorMessage;
        sendChangeMessage();
        return false;
    }

    plugin->instance = std::move (instance);
    plugin->description = description;
    plugin->loadError = {};
    plugin->deferredLoad = false;
    plugin->savedState.reset();
    preparePlugin (*plugin);
    publish();
    return true;
}

bool PluginChain::replacePlugin (int id, const juce::PluginDescription& wanted, juce::String& errorMessage)
{
    auto* plugin = findPlugin (id);

    if (plugin == nullptr)
        return false;

    // 先に新しいほうを読み込む（失敗したら元のまま）
    juce::PluginDescription description;
    auto instance = createInstance (wanted, description, errorMessage);

    if (instance == nullptr)
        return false;

    releaseInstance (*plugin);
    plugin->instance = std::move (instance);
    plugin->description = description;
    plugin->faulted = false;
    plugin->faultLogged = false;
    plugin->loadError = {};
    plugin->savedState.reset();
    plugin->setSnapshotJpeg ({}); // 別のプラグインなので、エディターの画面は撮り直す
    if (plugin->editorBounds.has_value())
        plugin->editorBounds = plugin->editorBounds->withSize (0, 0); // 別のプラグインなので、位置だけ引き継ぐ
    preparePlugin (*plugin);
    publish();
    return true;
}

void PluginChain::movePlugin (int id, int newIndex)
{
    const int from = indexOf (id);

    if (from < 0)
        return;

    newIndex = juce::jlimit (0, (int) plugins.size() - 1, newIndex);

    if (newIndex == from)
        return;

    auto plugin = std::move (plugins[(size_t) from]);
    plugins.erase (plugins.begin() + from);
    plugins.insert (plugins.begin() + newIndex, std::move (plugin));
    publish();
}

void PluginChain::setEnabled (int id, bool shouldBeEnabled)
{
    if (auto* plugin = findPlugin (id))
    {
        // セーフモードで読み込まずに戻した行は、ON にしたときに読み込む（読み込めなければ OFF のまま、理由を行に出す）
        if (shouldBeEnabled && plugin->deferredLoad && plugin->instance == nullptr)
        {
            juce::String error;
            if (! reloadPlugin (id, error))
            {
                sendChangeMessage();
                return;
            }
        }

        plugin->enabled = shouldBeEnabled;
        sendChangeMessage();
    }
}

void PluginChain::setBypassAll (bool shouldBypass)
{
    bypassAll = shouldBypass;
    sendChangeMessage();
}

int PluginChain::getLatencySamples (const Plugin& plugin)
{
    // AudioProcessor が持っている値を読むだけなので、例外を起こしたプラグインでも呼んでよい
    return plugin.instance != nullptr ? juce::jmax (0, plugin.instance->getLatencySamples()) : 0;
}

int PluginChain::getTotalLatencySamples() const
{
    if (bypassAll.load())
        return 0;

    int total = 0;
    for (const auto& plugin : plugins)
        if (plugin->enabled.load() && ! plugin->faulted.load())
            total += getLatencySamples (*plugin);

    return total;
}

juce::String PluginChain::formatLatency (int samples, double sampleRate)
{
    const double ms = sampleRate > 0.0 ? 1000.0 * samples / sampleRate : 0.0;
    return juce::String (ms, 1) + " ms (" + juce::String (samples) + " smp)";
}

//==============================================================================
namespace
{
bool creatingInstance = false; // プラグインを作っている途中（メッセージスレッドだけが使う。どのラックでも共通）
}

bool PluginChain::isCreatingInstance()
{
    return creatingInstance;
}

std::unique_ptr<juce::AudioPluginInstance> PluginChain::createInstance (const juce::PluginDescription& wanted, juce::PluginDescription& description,
                                                                        juce::String& errorMessage)
{
    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    auto instance = createInstanceUnlogged (wanted, description, errorMessage);
    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;

    if (instance == nullptr)
        AppLog::write ("plugin could not be loaded: " + wanted.name + " (" + wanted.fileOrIdentifier + ") | " + errorMessage);
    else if (elapsedMs >= 1000.0)
        AppLog::write ("plugin loaded slowly: " + description.name + " " + juce::String (elapsedMs / 1000.0, 1) + " s");
    return instance;
}

void PluginChain::logFault (const Plugin& plugin, const char* where) const
{
    AppLog::write (juce::String ("plugin stopped by an error in ") + where + ": " + plugin.description.name + " "
                   + plugin.description.version + " (" + plugin.description.fileOrIdentifier + ")");
}

std::unique_ptr<juce::AudioPluginInstance> PluginChain::createInstanceUnlogged (const juce::PluginDescription& wanted, juce::PluginDescription& description,
                                                                                juce::String& errorMessage)
{
    const juce::File file (wanted.fileOrIdentifier);

    // プラグインを作っている途中で、もう 1 つ作り始めない。初期化の中でメッセージを回すプラグイン（UJAM の UFX-REVERB）があり、
    // その間に届いた「追加」などでここに入れ子で入ると、同じプラグインを何十個も作って PC 全体が止まった（2026-11-06）
    if (creatingInstance)
    {
        errorMessage = TRANS ("Another plugin is still loading. Try again after it finishes.");
        return nullptr;
    }
    const juce::ScopedValueSetter<bool> creating (creatingInstance, true);

    // プラグイン安全チェックで、読み込むと Windows の描画資源を使い切ると分かったものは作らない
    // （使い切ると解放しても戻らず、画面が描けなくなって落ちる。プリセットの読み込みなどで入ってきたとき）
    if (PluginSafety::isBlocked (wanted))
    {
        errorMessage = PluginSafety::describe (*PluginSafety::findVerdict (wanted), wanted.name);
        return nullptr;
    }

    // 説明がそろっていれば、ファイルを調べ直さずにそのまま作る（findAllTypesForFile は中のプラグインを一つずつ作るので遅い。
    // WaveShell だと 30 秒以上かかることがある）
    if (wanted.uniqueId != 0 || wanted.deprecatedUid != 0)
    {
        description = wanted;
        if (auto instance = formatManager.createPluginInstance (description, (double) formatRate, (int) formatMaxFrames, errorMessage))
            return instance;
    }

    // 作れなかった（ファイルを作り直して説明が古くなったなど）、または説明がファイルの場所だけのときは、
    // ファイルを調べ直して同じプラグインを探す
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formatManager.getFormats())
        format->findAllTypesForFile (types, file.getFullPathName());

    if (types.isEmpty())
    {
        errorMessage = TRANS ("Cannot load this file as a VST3 plugin: %path%").replace ("%path%", file.getFullPathName());
        return nullptr;
    }

    const juce::PluginDescription* match = nullptr;
    for (auto* type : types)
        if ((wanted.uniqueId != 0 && type->uniqueId == wanted.uniqueId)
            || (wanted.deprecatedUid != 0 && type->deprecatedUid == wanted.deprecatedUid))
            match = type;
    for (auto* type : types)
        if (match == nullptr && wanted.name.isNotEmpty() && type->name == wanted.name)
            match = type;
    if (match == nullptr && (types.size() == 1 || (wanted.uniqueId == 0 && wanted.deprecatedUid == 0 && wanted.name.isEmpty())))
        match = types[0]; // 1 つしかない、または何を使っていたか分からない（古い形式の保存）

    if (match == nullptr)
    {
        errorMessage = TRANS ("%name% was not found in %path%").replace ("%name%", wanted.name).replace ("%path%", file.getFullPathName());
        return nullptr;
    }

    description = *match;
    errorMessage = {};

    if (PluginSafety::isBlocked (description))
    {
        errorMessage = PluginSafety::describe (*PluginSafety::findVerdict (description), description.name);
        return nullptr;
    }

    return formatManager.createPluginInstance (description, (double) formatRate, (int) formatMaxFrames, errorMessage);
}

// 音声スレッドの並びに入っていないプラグインに対してだけ呼ぶこと
void PluginChain::preparePlugin (Plugin& plugin)
{
    if (plugin.instance == nullptr || plugin.faulted.load())
        return; // 例外を起こしたものには触らない

    auto& instance = *plugin.instance;
    instance.setPlayHead (&playHead); // テンポを 0 として受け取らないように（FixedPlayHead）
    instance.releaseResources();

    // メインバスを処理の形式（ステレオなど）に。できなければステレオ、モノラルの順に試す
    if (! setMainBuses (instance, juce::AudioChannelSet::canonicalChannelSet ((int) formatChannels)))
        if (! setMainBuses (instance, juce::AudioChannelSet::stereo()))
            setMainBuses (instance, juce::AudioChannelSet::mono());

    if (! PluginGuard::prepareToPlay (&instance, (double) formatRate, (int) formatMaxFrames))
    {
        plugin.faulted = true;
        plugin.faultLogged = true;
        logFault (plugin, "prepareToPlay");
    }

    // 無音を 1 回処理させて、テンポなど（FixedPlayHead）を先に渡しておく。音が来る前（再生デバイスで無効のときなど）に
    // 画面を開いても、テンポを 0 のまま使わないように。まだ音声スレッドの並びに入っていないので、ここで呼んでよい
    if (! plugin.faulted.load())
    {
        juce::AudioBuffer<float> silence (juce::jmax ((int) formatChannels, pluginChannelCount (instance)), (int) formatMaxFrames);
        silence.clear();
        juce::MidiBuffer midi;
        const juce::ScopedLock callbackLock (instance.getCallbackLock());
        if (! PluginGuard::processBlock (&instance, &silence, &midi))
        {
            plugin.faulted = true;
            plugin.faultLogged = true;
            logFault (plugin, "the first silent block");
        }
    }

    // 無音時に休止するまでの長さ: 余韻（プラグインが報告するテイル）+ 2 秒（レイテンシーは処理のたびに足す）。
    // 余韻が 60 秒より長い（無限を含む）と報告するものは、音を出し続けるものとみなして休止させない
    const double tailSeconds = instance.getTailLengthSeconds();
    plugin.restAfterSamples = (tailSeconds >= 0.0 && tailSeconds <= 60.0)
                                  ? (juce::int64) ((tailSeconds + 2.0) * formatRate)
                                  : std::numeric_limits<juce::int64>::max();
    plugin.quietSamples = 0;
    plugin.resting = false;
}

void PluginChain::setAlwaysProcess (int id, bool shouldAlwaysProcess)
{
    if (auto* plugin = findPlugin (id))
    {
        plugin->alwaysProcess = shouldAlwaysProcess;
        sendChangeMessage();
    }
}

void PluginChain::setView (int id, Plugin::View view)
{
    if (auto* plugin = findPlugin (id); plugin != nullptr && plugin->view != view)
    {
        plugin->view = view;
        sendChangeMessage();
    }
}

void PluginChain::prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames)
{
    if (sampleRate == 0 || channels == 0 || maxFrames == 0)
        return;

    if (sampleRate == formatRate && channels == formatChannels && maxFrames == formatMaxFrames)
        return; // 形式が変わっていなければ準備し直さない（プラグインの状態を保つ）

    // 準備し直す間は、音声スレッドにプラグインを使わせない
    {
        const juce::SpinLock::ScopedLockType lock (chainLock);
        chain.clear();
        chainRate = 0;
    }

    formatRate = sampleRate;
    formatChannels = channels;
    formatMaxFrames = maxFrames;

    for (const auto& plugin : plugins)
        preparePlugin (*plugin);

    publish();
}

void PluginChain::publish()
{
    std::vector<Plugin*> order;
    int channelsNeeded = (int) formatChannels;

    for (const auto& plugin : plugins)
    {
        if (plugin->instance == nullptr)
            continue;

        order.push_back (plugin.get());
        channelsNeeded = juce::jmax (channelsNeeded, pluginChannelCount (*plugin->instance));
    }

    // 確保はロックの外で行う
    juce::AudioBuffer<float> buffer (channelsNeeded, (int) formatMaxFrames);
    buffer.clear();
    juce::AudioBuffer<float> safety ((int) formatChannels, (int) formatMaxFrames);
    safety.clear();

    {
        const juce::SpinLock::ScopedLockType lock (chainLock);
        chain.swap (order);
        std::swap (workBuffer, buffer);
        std::swap (safetyBuffer, safety);
        chainRate = formatRate;
        chainChannels = formatChannels;
        chainMaxFrames = formatMaxFrames;
    }

    sendChangeMessage();
}

//==============================================================================
void PluginChain::saveState (juce::ValueTree& parent) const
{
    parent.setProperty ("bypassAll", bypassAll.load(), nullptr);

    for (const auto& plugin : plugins)
    {
        juce::ValueTree child ("Plugin");
        child.setProperty ("enabled", plugin->enabled.load(), nullptr);
        child.setProperty ("alwaysProcess", plugin->alwaysProcess.load(), nullptr);
        child.setProperty ("view", (int) plugin->view, nullptr);
        if (! plugin->snapshotJpeg.isEmpty())
            child.setProperty ("snapshot", plugin->snapshotJpeg.toBase64Encoding(), nullptr);
        child.setProperty ("file", plugin->description.fileOrIdentifier, nullptr);
        if (plugin->editorBounds.has_value())
            child.setProperty ("editorBounds", plugin->editorBounds->toString(), nullptr); // "x y 幅 高さ"

        if (auto xml = plugin->description.createXml())
            child.setProperty ("description", xml->toString (juce::XmlElement::TextFormat().singleLine()), nullptr);

        // 読み込めていない・状態を取れないときは、控えてある状態を書く
        juce::MemoryBlock block;
        if (plugin->instance == nullptr || plugin->faulted.load() || ! PluginGuard::getState (plugin->instance.get(), &block))
            block = plugin->savedState;
        child.setProperty ("state", block.toBase64Encoding(), nullptr);

        parent.appendChild (child, nullptr);
    }
}

void PluginChain::restoreState (const juce::ValueTree& parent, juce::StringArray& errors, bool loadLater)
{
    bypassAll = (bool) parent.getProperty ("bypassAll", false);

    // 今のプラグインを外してから読み込む
    {
        const juce::SpinLock::ScopedLockType lock (chainLock);
        chain.clear();
    }
    for (auto& plugin : plugins)
        disposeInstance (std::move (plugin->instance), plugin->faulted.load());
    plugins.clear();

    for (const auto& child : parent)
    {
        if (! child.hasType ("Plugin"))
            continue;

        const juce::File file (child.getProperty ("file").toString());
        const bool enabled = child.getProperty ("enabled", true);
        const bool alwaysProcess = child.getProperty ("alwaysProcess", false);
        // なければ、または範囲外（以前あった「エディターの画面」= 5 など）なら、既定の「入力と出力のレベル」にする
        const int savedView = child.getProperty ("view", (int) Plugin::View::levelBars);
        const auto view = Plugin::isValidView (savedView) ? (Plugin::View) savedView : Plugin::View::levelBars;
        juce::MemoryBlock snapshotJpeg;
        snapshotJpeg.fromBase64Encoding (child.getProperty ("snapshot").toString());
        std::optional<juce::Rectangle<int>> editorBounds;
        if (child.hasProperty ("editorBounds"))
            editorBounds = juce::Rectangle<int>::fromString (child.getProperty ("editorBounds").toString());

        // 保存してある説明（どのプラグインか）。なければファイルの場所だけ
        juce::PluginDescription wanted;
        if (auto xml = juce::parseXML (child.getProperty ("description").toString()))
            if (! wanted.loadFromXml (*xml))
                wanted = {};
        wanted.fileOrIdentifier = file.getFullPathName();

        juce::MemoryBlock block;
        block.fromBase64Encoding (child.getProperty ("state").toString());

        // セーフモード（deferLoad）: 読み込まずに行だけ戻す（前回落ちた原因が、読み込み・状態の復元・準備のどれかでも落ちないように）。
        // ON にしたときに読み込む
        const bool deferLoad = child.getProperty ("deferLoad", false);
        const bool pending = loadLater && ! deferLoad; // あとで順に読み込む（loadNextPending）

        juce::String error;
        juce::PluginDescription description;
        auto instance = deferLoad || pending ? nullptr : createInstance (wanted, description, error);

        // 状態を戻す途中で例外が起きたら、そのインスタンスは使わずに手放し、読み込めなかった行として残す
        if (instance != nullptr && ! applyState (*instance, block))
        {
            disposeInstance (std::move (instance), true);
            error = TRANS ("The plugin crashed while restoring its settings.");
        }

        if (instance == nullptr)
        {
            // 行は残し、状態を控えておく（あとで「プラグインを更新」すれば読み込み直せる）
            if (! deferLoad && ! pending)
                errors.add (file.getFileName() + ": " + error);
            auto plugin = std::make_unique<Plugin>();
            plugin->id = nextPluginId++;
            plugin->enabled = deferLoad ? false : enabled;
            plugin->alwaysProcess = alwaysProcess;
            plugin->view = view;
            plugin->setSnapshotJpeg (snapshotJpeg);
            plugin->editorBounds = editorBounds;
            plugin->loadError = error;
            plugin->deferredLoad = deferLoad;
            plugin->pendingLoad = pending;
            plugin->savedState = block;
            plugin->description = wanted;
            plugins.push_back (std::move (plugin));
            continue;
        }

        auto plugin = makePlugin (std::move (instance), description, enabled);
        plugin->alwaysProcess = alwaysProcess;
        plugin->view = view;
        plugin->setSnapshotJpeg (snapshotJpeg);
        plugin->editorBounds = editorBounds;
        plugins.push_back (std::move (plugin));
    }

    publish();
}

bool PluginChain::loadNextPending (juce::StringArray& errors)
{
    for (const auto& plugin : plugins)
    {
        if (! plugin->pendingLoad)
            continue;

        plugin->pendingLoad = false;
        juce::String error;
        if (! reloadPlugin (plugin->id, error)) // 控えてある状態を戻して準備する。失敗しても行は残す（「プラグインを更新」で読み込み直せる）
            errors.add (juce::File (plugin->description.fileOrIdentifier).getFileName() + ": " + error);
        return true;
    }
    return false;
}

int PluginChain::countPendingLoads() const
{
    return (int) std::count_if (plugins.begin(), plugins.end(), [] (const auto& plugin) { return plugin->pendingLoad; });
}

//==============================================================================
void PluginChain::process (float* const* channelData, uint32_t channels, uint32_t frames, uint32_t sampleRate)
{
    if (bypassAll.load (std::memory_order_relaxed) || suspended.load (std::memory_order_relaxed))
        return;

    const juce::SpinLock::ScopedLockType lock (chainLock);

    const bool usable = sampleRate == chainRate && channels == chainChannels && frames <= chainMaxFrames
                     && (int) channels <= workBuffer.getNumChannels() && (int) frames <= workBuffer.getNumSamples()
                     && (int) channels <= safetyBuffer.getNumChannels() && (int) frames <= safetyBuffer.getNumSamples();

    if (! usable || chain.empty())
        return;

    auto* const* work = workBuffer.getArrayOfWritePointers();
    for (uint32_t ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::copy (work[ch], channelData[ch], (int) frames);

    const bool restWhenSilentNow = restWhenSilent.load (std::memory_order_relaxed);

    for (auto* plugin : chain)
    {
        // OFF のプラグインと、例外で止まったプラグインには処理させない
        if (! plugin->enabled.load (std::memory_order_relaxed) || plugin->faulted.load (std::memory_order_relaxed))
            continue;

        auto& instance = *plugin->instance;
        const int numChannels = juce::jmax ((int) channels, pluginChannelCount (instance));

        if (numChannels > workBuffer.getNumChannels())
            continue;

        // 無音時の休止: 入力が無音で、入力も出力も無音の状態が十分続いていれば処理させない
        // （そのときは出力も無音のはずなので、無音の入力をそのまま次へ渡す）
        const bool inputSilent = isSilent (work, channels, frames);
        const auto restAfter = plugin->restAfterSamples.load (std::memory_order_relaxed);
        const auto latency = (juce::int64) juce::jmax (0, instance.getLatencySamples());
        if (restWhenSilentNow && inputSilent && ! plugin->alwaysProcess.load (std::memory_order_relaxed)
            && restAfter != std::numeric_limits<juce::int64>::max() && plugin->quietSamples >= restAfter + latency)
        {
            plugin->resting.store (true, std::memory_order_relaxed);
            continue;
        }
        plugin->resting.store (false, std::memory_order_relaxed);

        // サイドチェーンなど、処理の形式より多いチャンネルは無音にしておく
        for (int ch = (int) channels; ch < numChannels; ++ch)
            juce::FloatVectorOperations::clear (work[ch], (int) frames);

        // 例外に備えて、このプラグインに渡す前の音を控えておく
        for (uint32_t ch = 0; ch < channels; ++ch)
            safetyBuffer.copyFrom ((int) ch, 0, work[ch], (int) frames);

        juce::AudioBuffer<float> view (work, numChannels, (int) frames);
        midiBuffer.clear();

        const juce::ScopedLock callbackLock (instance.getCallbackLock());

        // 処理の重さ（行のメーター用）: processBlock にかかった時間を周期の長さで割る
        const auto startTicks = juce::Time::getHighResolutionTicks();
        const bool processed = instance.isSuspended() || PluginGuard::processBlock (&instance, &view, &midiBuffer);
        if (sampleRate > 0)
            plugin->tap.recordLoad ((float) (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks)
                                             * sampleRate / frames));

        if (! processed)
        {
            // 例外が起きた。以後このプラグインは通さず、途中まで書かれた音は捨てて渡す前の音に戻す
            plugin->faulted = true;
            faultPending = true;
            triggerAsyncUpdate();

            for (uint32_t ch = 0; ch < channels; ++ch)
                juce::FloatVectorOperations::copy (work[ch], safetyBuffer.getReadPointer ((int) ch), (int) frames);
        }
        else
        {
            // 行の表示（増減のメーター）のために、渡す前の音（safetyBuffer）と処理した後の音を控える
            plugin->tap.push (safetyBuffer.getArrayOfReadPointers(), work, (int) channels, (int) frames, sampleRate);
        }

        // 入力も出力も無音なら無音の長さを延ばし、どちらかに音があれば数え直す
        if (inputSilent && isSilent (work, channels, frames))
            plugin->quietSamples = juce::jmin (plugin->quietSamples + (juce::int64) frames, std::numeric_limits<juce::int64>::max() / 2);
        else
            plugin->quietSamples = 0;
    }

    for (uint32_t ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::copy (channelData[ch], work[ch], (int) frames);
}
