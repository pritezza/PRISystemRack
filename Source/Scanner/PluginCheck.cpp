#include "PluginCheck.h"

#include <windows.h>

#include <map>
#include <set>

namespace
{
juce::String ja (const char* text)
{
    return juce::String::fromUTF8 (text);
}

void pumpMessages()
{
    MSG msg;
    while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
}

double nowMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}

//==============================================================================
// --check
constexpr double checkRate = 48000.0;
constexpr int checkBlock = 480; // 10 ms
constexpr int checkChannels = 2; // 処理の形式のチャンネル数（APO から来る形式。ふつうはステレオ）
constexpr int gdiThreshold = 2000; // 本体のプラグイン安全チェック（PluginSafety::gdiThreshold）と同じ

// 本体と同じ再生位置の情報（120 BPM、4/4、止まっている。PluginChain の FixedPlayHead のまね）
struct StoppedPlayHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (120.0);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setTimeInSamples (0);
        info.setTimeInSeconds (0.0);
        info.setPpqPosition (0.0);
        info.setPpqPositionOfLastBarStart (0.0);
        info.setIsPlaying (false);
        return info;
    }
};

bool setMainBuses (juce::AudioPluginInstance& plugin, const juce::AudioChannelSet& set)
{
    auto layout = plugin.getBusesLayout();
    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference (0) = set;
    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = set;
    return plugin.setBusesLayout (layout);
}

struct ProcessStats
{
    bool finite = true;
    double peak = 0.0;
    double maxMs = 0.0;
};

// ノイズ（-20 dBFS ほど）を blocks 回渡して処理させる。最初の 5 回は時間を測らない（初回だけ重いものが多い）
ProcessStats runBlocks (juce::AudioPluginInstance& instance, int blocks, juce::Random& random)
{
    const int channels = juce::jmax (2, instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels());
    const int outputs = juce::jmax (1, instance.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer (channels, checkBlock);
    juce::MidiBuffer midi;
    ProcessStats stats;

    for (int b = 0; b < blocks; ++b)
    {
        // 本体と同じく、ノイズはメインの 2ch（処理の形式）にだけ入れ、それより後ろ（サイドチェーンなど）は無音にする
        buffer.clear();
        for (int ch = 0; ch < juce::jmin (checkChannels, channels); ++ch)
            for (int i = 0; i < checkBlock; ++i)
                buffer.setSample (ch, i, 0.17f * (random.nextFloat() * 2.0f - 1.0f));
        midi.clear();

        const auto t0 = nowMs();
        {
            const juce::ScopedLock lock (instance.getCallbackLock());
            instance.processBlock (buffer, midi);
        }
        if (b >= 5)
            stats.maxMs = juce::jmax (stats.maxMs, nowMs() - t0);

        for (int ch = 0; ch < juce::jmin (outputs, channels); ++ch)
        {
            const float* data = buffer.getReadPointer (ch);
            for (int i = 0; i < checkBlock; ++i)
            {
                if (! std::isfinite (data[i]))
                    stats.finite = false;
                else
                    stats.peak = juce::jmax (stats.peak, (double) std::abs (data[i]));
            }
        }

        if (b % 10 == 0)
            pumpMessages();
    }
    return stats;
}

//==============================================================================
// --check-all

struct ChildResult
{
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
};

// 子のプロセスを起動して終わるまで待つ。子が起動した孫のプロセス（プラグインの認証や登録の補助など）も一緒に 1 つのジョブに入れ、
// 終わったら（時間切れのときも）ジョブごと終わらせる。孫が調査の最後まで残り続けないように
ChildResult runChild (const juce::StringArray& args, int timeoutMs)
{
    ChildResult result;

    juce::String commandLine;
    for (const auto& arg : args)
        commandLine << (commandLine.isEmpty() ? "" : " ") << arg.quoted();
    std::wstring command (commandLine.toWideCharPointer());

    const auto job = CreateJobObjectW (nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION process {};
    if (! CreateProcessW (nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &process))
    {
        if (job != nullptr)
            CloseHandle (job);
        return result;
    }

    if (job != nullptr)
        AssignProcessToJobObject (job, process.hProcess); // 動き出す前に入れる（孫もこのジョブに入る）
    ResumeThread (process.hThread);
    result.started = true;

    if (WaitForSingleObject (process.hProcess, (DWORD) timeoutMs) == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess (process.hProcess, &code);
        result.exitCode = (int) code;
    }
    else
    {
        result.timedOut = true;
        TerminateProcess (process.hProcess, 1);
        WaitForSingleObject (process.hProcess, 5000);
    }

    if (job != nullptr)
    {
        TerminateJobObject (job, 1);
        CloseHandle (job);
    }
    CloseHandle (process.hThread);
    CloseHandle (process.hProcess);
    return result;
}

constexpr int scanTimeoutMs = 600000;  // 一覧を読む（WaveShell は中のプラグインを全部作るので長い）
constexpr int checkTimeoutMs = 120000; // 1 つ試す（認証の画面などで止まったら時間切れ）

struct Record
{
    juce::String type;   // FILE か PLUGIN
    juce::String key;    // FILE: パス|大きさ|更新日時。PLUGIN: それ + | + プラグインの識別子
    juce::String file;   // .vst3 のパス
    juce::String name, vendor, version, category;
    bool instrument = false;
    juce::String status; // ok / warn / fail
    juce::String reason; // 何が起きたか（日本語）
    juce::String notes;  // 補足（日本語）
    double seconds = 0.0;

    static juce::String clean (const juce::String& s) { return s.replaceCharacters ("\t\r\n|", "   /"); }

    juce::String toLine() const
    {
        return juce::StringArray { type, key, clean (file), clean (name), clean (vendor), clean (version), clean (category),
                                   instrument ? "1" : "0", status, clean (reason), clean (notes), juce::String (seconds, 1) }
            .joinIntoString ("\t");
    }

    static bool fromLine (const juce::String& line, Record& r)
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 12)
            return false;
        r.type = f[0];
        r.key = f[1];
        r.file = f[2];
        r.name = f[3];
        r.vendor = f[4];
        r.version = f[5];
        r.category = f[6];
        r.instrument = f[7] == "1";
        r.status = f[8];
        r.reason = f[9];
        r.notes = f[10];
        r.seconds = f[11].getDoubleValue();
        return true;
    }
};

juce::File foldersFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("PRISystemRack").getChildFile ("PluginFolders.xml");
}

juce::StringArray overrideFolders; // 試験用: --check-all の 3 つ目の引数で調べるフォルダーを指定したとき

juce::StringArray pluginFolders()
{
    if (! overrideFolders.isEmpty())
        return overrideFolders;

    juce::StringArray folders;
    if (const auto xml = juce::parseXMLIfTagMatches (foldersFile(), "PLUGINFOLDERS"))
        for (auto* child : xml->getChildWithTagNameIterator ("FOLDER"))
            folders.addIfNotAlreadyThere (child->getStringAttribute ("path"), true);
    else
        folders.add ("C:\\Program Files\\Common Files\\VST3");
    folders.removeEmptyStrings();
    return folders;
}

// ファイルが変わったかを見るための印（フォルダー形式の .vst3 は中の DLL の大きさと更新日時）
juce::String fileKeyOf (const juce::File& file)
{
    auto target = file;
    if (file.isDirectory())
    {
        const auto inner = file.getChildFile ("Contents").getChildFile ("x86_64-win");
        const auto found = inner.findChildFiles (juce::File::findFiles, false, "*.vst3");
        if (! found.isEmpty())
            target = found.getFirst();
    }
    return file.getFullPathName() + "|" + juce::String (target.getSize()) + "|" + juce::String (target.getLastModificationTime().toMilliseconds());
}

// 中にプラグインが見つからなかったファイルの理由（32 ビット版か、Windows の本体がないか）
juce::String describeEmptyFile (const juce::File& file)
{
    auto binary = file;
    if (file.isDirectory())
    {
        const auto found = file.getChildFile ("Contents").findChildFiles (juce::File::findFiles, true, "*.vst3");
        if (found.isEmpty())
            return ja ("中にプラグインの本体（Windows の DLL）がない");
        binary = found.getFirst();
    }

    juce::MemoryBlock header;
    if (juce::FileInputStream in (binary); in.openedOk())
        in.readIntoMemoryBlock (header, 4096);
    const auto* bytes = static_cast<const juce::uint8*> (header.getData());
    if (header.getSize() >= 0x40 && bytes[0] == 'M' && bytes[1] == 'Z')
    {
        const auto pe = (size_t) juce::ByteOrder::littleEndianInt (bytes + 0x3C);
        if (pe + 6 <= header.getSize() && bytes[pe] == 'P' && bytes[pe + 1] == 'E')
            if (juce::ByteOrder::littleEndianShort (bytes + pe + 4) == 0x014C) // IMAGE_FILE_MACHINE_I386
                return ja ("32 ビット版（64 ビットのアプリでは読み込めない）");
    }
    return ja ("中にプラグインが見つからない");
}

juce::String hex (int code)
{
    return "0x" + juce::String::toHexString ((juce::uint32) code).toUpperCase().paddedLeft ('0', 8);
}

juce::String stageName (const juce::String& stage)
{
    if (stage == "create")    return ja ("作るところ");
    if (stage == "prepare")   return ja ("処理の準備（prepareToPlay）");
    if (stage == "process")   return ja ("音を通すところ");
    if (stage == "state")     return ja ("設定の保存と復元");
    if (stage == "reprocess") return ja ("設定を戻したあと音を通すところ");
    if (stage == "release")   return ja ("解放するところ");
    return stage.isNotEmpty() ? stage : ja ("起動直後");
}

// 1 つ試した結果（<CHECK> と終了のしかた）を、○△× と理由にする
void judge (Record& r, const juce::XmlElement* check, bool finished, bool timedOut, int exitCode)
{
    const auto stage = check != nullptr ? check->getStringAttribute ("stage") : juce::String();
    juce::StringArray notes;

    juce::ignoreUnused (finished);
    if (timedOut)
    {
        r.status = "timeout"; // × と同じに数える。続きから起動するとき -RetryTimeouts を付ければ試し直す
        r.reason = stageName (stage) + ja (" で ") + juce::String (checkTimeoutMs / 1000) + ja (" 秒たっても終わらない（認証や登録の画面で止まっている可能性）");
    }
    else if (check == nullptr)
    {
        r.status = "fail";
        r.reason = ja ("結果を書けずに終わった（終了コード ") + hex (exitCode) + ja ("）");
    }
    // ここから下は、どの段階まで進んだか（stage）で判定する。落ちたとき（finished が false）も stage は残っている
    else if (stage == "createFailed")
    {
        r.status = "fail";
        const auto error = check->getStringAttribute ("error");
        r.reason = ja ("作れない") + (error.isNotEmpty() ? ja (": ") + error : juce::String());
    }
    else if (stage != "done")
    {
        if (stage == "release")
        {
            r.status = "warn";
            r.reason = ja ("使えるが、解放するところで落ちる（終了コード ") + hex (exitCode) + ja ("）。プラグインを外す・プリセットを切り替えるときにアプリごと落ちるおそれ");
        }
        else
        {
            r.status = "fail";
            r.reason = stageName (stage) + ja (" で落ちた（終了コード ") + hex (exitCode) + ja ("）");
        }
    }
    else if (! check->getBoolAttribute ("finite", true))
    {
        r.status = "fail";
        r.reason = ja ("音を通すと NaN・無限大を出す");
    }
    else if (! check->getBoolAttribute ("finiteAfterState", true))
    {
        r.status = "fail";
        r.reason = ja ("設定を戻したあと NaN・無限大を出す");
    }
    else if (check->getIntAttribute ("gdi") >= gdiThreshold)
    {
        r.status = "fail";
        r.reason = ja ("Windows の描画資源（GDI）を 1 つで ") + juce::String (check->getIntAttribute ("gdi"))
                 + ja (" 個使う。アプリのプラグイン安全チェックで読み込まれない");
    }
    else if (check->getDoubleAttribute ("peak") < 1.0e-6 && ! r.instrument)
    {
        r.status = "warn";
        r.reason = ja ("音を通すと無音になる（体験版・未認証で消音している、入力を使わないなどの可能性）");
    }
    else
    {
        r.status = "ok";
    }

    if (check != nullptr)
    {
        if (const double s = check->getDoubleAttribute ("createSeconds"); s >= 10.0)
            notes.add (ja ("作るのに ") + juce::String (s, 1) + ja (" 秒"));
        if (const double ms = check->getDoubleAttribute ("maxProcessMs"); ms > 10.0)
            notes.add (ja ("処理が重い（10 ms 分の処理に最大 ") + juce::String (ms, 1) + ja (" ms）"));
        if (const int latency = check->getIntAttribute ("latency"); latency > 0)
            notes.add (ja ("レイテンシー ") + juce::String (latency) + " smp");
        if (check->hasAttribute ("inputs") && check->getIntAttribute ("inputs") == 0 && ! r.instrument)
            notes.add (ja ("入力なし"));
        if (stage == "done" && check->getIntAttribute ("stateBytes") == 0)
            notes.add (ja ("設定データが空"));
    }
    if (r.instrument)
        notes.insert (0, ja ("楽器"));
    r.notes = notes.joinIntoString (ja ("、"));
}

juce::String cell (const juce::String& s)
{
    return s.replace ("|", "/").replace ("\n", " ");
}

void writeReport (const juce::File& reportFile, const std::vector<Record>& records, int filesDone, int filesTotal, bool finished,
                  const juce::String& startedAt)
{
    std::vector<const Record*> ok, warn, fail, scanFailed, skipped;
    for (const auto& r : records)
    {
        if (r.type == "PLUGIN")
            (r.status == "ok" ? ok : r.status == "warn" ? warn : fail).push_back (&r);
        else if (r.type == "FILE" && (r.status == "fail" || r.status == "timeout"))
            scanFailed.push_back (&r);
        else if (r.type == "FILE" && r.status == "skip")
            skipped.push_back (&r);
    }

    auto sortByName = [] (std::vector<const Record*>& v)
    {
        std::sort (v.begin(), v.end(), [] (const Record* a, const Record* b)
        {
            const int c = a->vendor.compareIgnoreCase (b->vendor);
            return c != 0 ? c < 0 : a->name.compareIgnoreCase (b->name) < 0;
        });
    };
    sortByName (ok);
    sortByName (warn);
    sortByName (fail);

    juce::String md;
    md << ja ("# VST3 プラグインの読み込み調査\n\n");
    md << ja ("このPCにある VST3 プラグインを 1 つずつ、PRI System Rack と同じ手順で読み込めるかを試した結果。\n\n");
    md << ja ("- 状態: ") << (finished ? ja ("**完了**") : ja ("**調査中**（途中で止めても、もう一度起動すれば続きから）"))
       << ja ("　ファイル ") << filesDone << " / " << filesTotal << "\n";
    md << ja ("- 開始: ") << startedAt << ja ("　最終更新: ") << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") << "\n";
    md << ja ("- 調べたフォルダー: ") << pluginFolders().joinIntoString (", ") << "\n\n";

    md << ja ("## 集計\n\n");
    md << ja ("| 判定 | 数 |\n|---|---|\n");
    md << ja ("| ○ 使える | ") << (int) ok.size() << " |\n";
    md << ja ("| △ 要確認 | ") << (int) warn.size() << " |\n";
    md << ja ("| × 使えない | ") << (int) fail.size() << " |\n";
    md << ja ("| 一覧を読めなかったファイル | ") << (int) scanFailed.size() << " |\n";
    md << ja ("| 対象外にしたファイル | ") << (int) skipped.size() << " |\n\n";

    auto table = [&md] (const std::vector<const Record*>& list, bool withReason)
    {
        md << (withReason ? ja ("| メーカー | プラグイン | 理由 | 補足 | ファイル |\n|---|---|---|---|---|\n")
                          : ja ("| メーカー | プラグイン | 補足 | ファイル |\n|---|---|---|---|\n"));
        for (const auto* r : list)
        {
            md << "| " << cell (r->vendor) << " | " << cell (r->name) << " | ";
            if (withReason)
                md << cell (r->reason) << " | ";
            md << cell (r->notes) << " | " << cell (juce::File (r->file).getFileName()) << " |\n";
        }
        md << "\n";
    };

    md << ja ("## × 使えない\n\n");
    if (fail.empty()) md << ja ("なし\n\n"); else table (fail, true);

    md << ja ("## △ 要確認\n\n");
    if (warn.empty()) md << ja ("なし\n\n"); else table (warn, true);

    md << ja ("## 一覧を読めなかったファイル\n\n");
    if (scanFailed.empty())
    {
        md << ja ("なし\n\n");
    }
    else
    {
        md << ja ("| ファイル | 理由 |\n|---|---|\n");
        for (const auto* r : scanFailed)
            md << "| " << cell (r->file) << " | " << cell (r->reason) << " |\n";
        md << "\n";
    }

    md << ja ("## 対象外にしたファイル\n\n");
    if (skipped.empty())
    {
        md << ja ("なし\n\n");
    }
    else
    {
        md << ja ("作業フォルダーの skip.txt で対象外にしたファイル。対象外にする前に試した分は、上の表に入っている。\n\n");
        for (const auto* r : skipped)
            md << "- " << cell (r->file) << "\n";
        md << "\n";
    }

    md << ja ("## ○ 使える\n\n<details><summary>") << (int) ok.size() << ja (" 個（開く）</summary>\n\n");
    table (ok, false);
    md << "</details>\n\n";

    md << ja ("## 試した内容\n\n")
       << ja ("プラグインごとに別のプロセス（PRISystemRackScanner.exe --check）で、次の順に試した。どこかで落ちたり止まったりしても、ほかのプラグインの調査は続く。\n\n")
       << ja ("1. 作る（48 kHz、1 回 480 サンプル。本体と同じ）\n")
       << ja ("2. 準備する（本体と同じく、テンポなどの情報を渡し、メインの入出力をステレオ（だめならモノラル）にして prepareToPlay）\n")
       << ja ("3. ノイズ（-20 dBFS ほど）を約 1 秒（100 回）通し、出力に NaN・無限大がないか、無音でないかを見る\n")
       << ja ("4. 設定を保存して（getStateInformation）そのまま戻し（setStateInformation）、もう一度 20 回通す\n")
       << ja ("5. 解放する\n\n")
       << ja ("1 つ ") << (checkTimeoutMs / 1000) << ja (" 秒で終わらなければ時間切れ（×。`Run-PluginCheck.ps1 -RetryTimeouts` で続きから起動すると、時間切れのものだけ試し直す）。")
       << ja ("画面（エディター）は開いていない。")
       << ja ("音はメモリの上で処理しただけで、スピーカーには出していない。\n\n")
       << ja ("判定:\n\n")
       << ja ("- ○ 使える: 最後まで問題なく終わった\n")
       << ja ("- △ 要確認: 最後まで動いたが、無音になる・解放で落ちるなど、気になる点がある\n")
       << ja ("- × 使えない: 作れない、途中で落ちる・止まる、NaN を出す、描画資源を使いすぎる（アプリが読み込まない）\n\n")
       << ja ("やり直し・続き: `Tools/PluginCheck/Run-PluginCheck.ps1` を実行する（結果は `%LOCALAPPDATA%\\PRISystemRack\\PluginCheck\\results.tsv`。")
       << ja ("消すと最初から）。")
       << ja ("ファイルを対象外にするには、同じフォルダーの `skip.txt` にファイル名を 1 行に 1 つ書く（調べている最中でも、次のプラグインに移るときに効く）。\n");

    reportFile.replaceWithText (md, false, false, "\n");
}
} // namespace

namespace PluginCheck
{
int checkOne (const juce::File& descriptionFile, const juce::File& outputFile)
{
    juce::PluginDescription description;
    const auto xml = juce::parseXML (descriptionFile);
    if (xml == nullptr || ! description.loadFromXml (*xml))
        return 2;

    juce::XmlElement report ("CHECK");
    auto stage = [&] (const char* name)
    {
        report.setAttribute ("stage", name);
        report.writeTo (outputFile);
    };

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    // 1. 作る
    stage ("create");
    const auto process = GetCurrentProcess();
    const int gdiBefore = (int) GetGuiResources (process, GR_GDIOBJECTS);
    const auto createStart = nowMs();
    juce::String error;
    auto instance = formats.createPluginInstance (description, checkRate, checkBlock, error);
    report.setAttribute ("createSeconds", (nowMs() - createStart) / 1000.0);
    report.setAttribute ("gdi", (int) GetGuiResources (process, GR_GDIOBJECTS) - gdiBefore);
    if (instance == nullptr)
    {
        report.setAttribute ("error", error);
        stage ("createFailed");
        return 1;
    }
    pumpMessages();

    // 2. 準備する（PluginChain::preparePlugin と同じ手順）
    stage ("prepare");
    static StoppedPlayHead playHead;
    instance->setPlayHead (&playHead);
    instance->releaseResources();
    if (! setMainBuses (*instance, juce::AudioChannelSet::canonicalChannelSet (checkChannels)))
        if (! setMainBuses (*instance, juce::AudioChannelSet::stereo()))
            setMainBuses (*instance, juce::AudioChannelSet::mono());
    instance->prepareToPlay (checkRate, checkBlock);
    report.setAttribute ("inputs", instance->getTotalNumInputChannels());
    report.setAttribute ("outputs", instance->getTotalNumOutputChannels());
    report.setAttribute ("latency", instance->getLatencySamples());

    // 本体と同じく、準備の直後に無音を 1 回処理させる（テンポなどを先に渡す）
    {
        juce::AudioBuffer<float> silence (juce::jmax (checkChannels, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels()), checkBlock);
        silence.clear();
        juce::MidiBuffer midi;
        const juce::ScopedLock lock (instance->getCallbackLock());
        instance->processBlock (silence, midi);
    }
    pumpMessages();

    // 3. 音を通す
    stage ("process");
    juce::Random random (1);
    const auto first = runBlocks (*instance, 100, random);
    report.setAttribute ("finite", first.finite);
    report.setAttribute ("peak", first.peak);
    report.setAttribute ("maxProcessMs", first.maxMs);

    // 4. 設定を保存して戻し、もう一度通す
    stage ("state");
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    report.setAttribute ("stateBytes", (int) state.getSize());
    if (state.getSize() > 0)
        instance->setStateInformation (state.getData(), (int) state.getSize());
    pumpMessages();

    stage ("reprocess");
    const auto second = runBlocks (*instance, 20, random);
    report.setAttribute ("finiteAfterState", second.finite);

    // 5. 解放する
    stage ("release");
    instance->releaseResources();
    instance.reset();
    pumpMessages();

    stage ("done");
    return 0;
}

int checkAll (const juce::File& workFolder, const juce::File& reportFile, const juce::String& folder)
{
    if (folder.isNotEmpty())
        overrideFolders.add (folder);
    SetConsoleOutputCP (CP_UTF8);
    workFolder.createDirectory();

    // 子のプロセス（一覧を読む・1 つ試す）は、この処理が終わったら（窓を閉じたときも）一緒に終わらせる
    if (const auto job = CreateJobObjectW (nullptr, nullptr))
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
        AssignProcessToJobObject (job, GetCurrentProcess());
    }

    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto resultsFile = workFolder.getChildFile ("results.tsv");
    const auto startedFile = workFolder.getChildFile ("started.txt");
    const auto tempDescription = workFolder.getChildFile ("description.xml");
    const auto tempOutput = workFolder.getChildFile ("output.xml");

    if (! startedFile.existsAsFile())
        startedFile.replaceWithText (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S"));
    const auto startedAt = startedFile.loadFileAsString().trim();

    // これまでの結果（同じ key が何度もあれば最後のもの）
    std::vector<Record> records;
    std::map<juce::String, size_t> byKey;
    {
        juce::StringArray lines;
        resultsFile.readLines (lines);
        for (const auto& line : lines)
        {
            Record r;
            if (! Record::fromLine (line, r))
                continue;
            if (r.status == "fail" && r.reason.contains (ja ("秒たっても終わらない")))
                r.status = "timeout"; // 時間切れを別の状態にする前の記録
            if (const auto it = byKey.find (r.key); it != byKey.end())
                records[it->second] = r;
            else
            {
                byKey[r.key] = records.size();
                records.push_back (r);
            }
        }
    }

    auto add = [&] (const Record& r)
    {
        resultsFile.appendText (r.toLine() + "\n", false, false, "\n");
        if (const auto it = byKey.find (r.key); it != byKey.end())
            records[it->second] = r;
        else
        {
            byKey[r.key] = records.size();
            records.push_back (r);
        }
    };

    // 本体の「VST3 フォルダー」の設定どおりに .vst3 を集める（ファイルとフォルダー形式の両方。下のフォルダーも）
    juce::FileSearchPath path;
    for (const auto& searchFolder : pluginFolders())
        path.add (juce::File (searchFolder));
    auto files = juce::VST3PluginFormat().searchPathsForPlugins (path, true, false);
    files.sortNatural();

    // ファイルごとの印（変わったかを見る）は最初に 1 回だけ求める
    juce::StringArray fileKeys;
    for (const auto& f : files)
        fileKeys.add (fileKeyOf (juce::File (f)));
    const std::set<juce::String> currentFileKeys (fileKeys.begin(), fileKeys.end());

    // 時間切れ（timeout）を試し直すか（Run-PluginCheck.ps1 -RetryTimeouts が環境変数で渡す）
    const bool retryTimeouts = juce::SystemStats::getEnvironmentVariable ("PRI_CHECK_RETRY_TIMEOUTS", {}) == "1";

    // 試し終わっているか。対象外（skip）だったものは試し直す。-RetryTimeouts なら時間切れのものも
    auto isDone = [&] (const juce::String& key)
    {
        const auto it = byKey.find (key);
        if (it == byKey.end())
            return false;
        const auto& status = records[it->second].status;
        return status != "skip" && ! (retryTimeouts && status == "timeout");
    };

    // ファイルが終わっているか（-RetryTimeouts なら、中に時間切れのプラグインがあるものは終わっていないとみなす）
    auto isFileDone = [&] (const juce::String& fileKey)
    {
        if (! isDone (fileKey))
            return false;
        if (retryTimeouts)
            for (const auto& r : records)
                if (r.type == "PLUGIN" && r.status == "timeout" && r.key.startsWith (fileKey + "|"))
                    return false;
        return true;
    };

    // 今あるファイルの結果だけを .md に出す（消したファイルや、変わる前の結果は出さない）
    auto currentRecords = [&]
    {
        std::vector<Record> current;
        for (const auto& r : records)
        {
            const auto fileKey = r.type == "FILE" ? r.key : r.key.upToLastOccurrenceOf ("|", false, false);
            if (currentFileKeys.count (fileKey) != 0)
                current.push_back (r);
        }
        return current;
    };

    auto filesDoneCount = [&]
    {
        int done = 0;
        for (const auto& key : fileKeys)
            if (byKey.count (key) != 0)
                ++done;
        return done;
    };

    std::printf ("VST3 check: %d files. Results: %s\n", files.size(), reportFile.getFullPathName().toRawUTF8());
    std::printf ("Close this window to stop; run again to continue.%s\n\n", retryTimeouts ? " Retrying timed-out ones." : "");
    std::fflush (stdout);

    // 対象外にするファイル（作業フォルダーの skip.txt。1 行に 1 つ、ファイル名かフルパス）。動いている間に書き足しても、
    // 次のファイル・次のプラグインに移るときに読み直すので効く
    const auto skipFile = workFolder.getChildFile ("skip.txt");
    auto isSkipped = [&skipFile] (const juce::File& f)
    {
        juce::StringArray lines;
        skipFile.readLines (lines);
        for (const auto& line : lines)
            if (const auto entry = line.trim().unquoted(); entry.isNotEmpty()
                && (f.getFileName().equalsIgnoreCase (entry) || f.getFullPathName().equalsIgnoreCase (entry)))
                return true;
        return false;
    };
    auto recordSkip = [&] (const juce::File& f, const juce::String& fileKey)
    {
        if (const auto it = byKey.find (fileKey); it != byKey.end() && records[it->second].status == "skip")
            return;
        Record r;
        r.type = "FILE";
        r.key = fileKey;
        r.file = f.getFullPathName();
        r.status = "skip";
        r.reason = ja ("対象外にした（skip.txt）");
        add (r);
        std::printf ("    skipped (skip.txt)\n");
        std::fflush (stdout);
        writeReport (reportFile, currentRecords(), filesDoneCount(), files.size(), false, startedAt);
    };

    for (int i = 0; i < files.size(); ++i)
    {
        const juce::File file (files[i]);
        const auto& fileKey = fileKeys[i];
        if (isSkipped (file))
        {
            recordSkip (file, fileKey);
            continue;
        }
        if (isFileDone (fileKey))
            continue; // このファイルは終わっている（対象外だったものは、skip.txt から外せば続きから試す）

        SetConsoleTitleW ((juce::String ("VST3 check ") + juce::String (i + 1) + " / " + juce::String (files.size())).toWideCharPointer());
        std::printf ("[%d/%d] %s\n", i + 1, files.size(), file.getFileName().toRawUTF8());
        std::fflush (stdout);

        // 一覧を読む（別のプロセス）
        Record fileRecord;
        fileRecord.type = "FILE";
        fileRecord.key = fileKey;
        fileRecord.file = file.getFullPathName();

        const auto listFile = workFolder.getChildFile ("list.xml");
        listFile.deleteFile();
        const auto scanStart = nowMs();
        const auto scan = runChild ({ self.getFullPathName(), file.getFullPathName(), listFile.getFullPathName() }, scanTimeoutMs);
        bool scanned = false;
        juce::String scanError;
        if (! scan.started)
            scanError = ja ("一覧を読むプロセスを起動できない");
        else if (scan.timedOut)
            scanError = ja ("一覧を読むのに ") + juce::String (scanTimeoutMs / 1000) + ja (" 秒たっても終わらない");
        else if (scan.exitCode != 0)
            scanError = ja ("一覧を読む途中で落ちた（終了コード ") + hex (scan.exitCode) + ja ("）");
        else
            scanned = true;
        fileRecord.seconds = (nowMs() - scanStart) / 1000.0;

        juce::Array<juce::PluginDescription> types;
        if (scanned)
            if (const auto list = juce::parseXMLIfTagMatches (listFile, "PLUGINS"))
                for (auto* child : list->getChildIterator())
                {
                    juce::PluginDescription d;
                    if (d.loadFromXml (*child))
                        types.add (d);
                }

        if (! scanned || types.isEmpty())
        {
            fileRecord.status = scan.timedOut ? "timeout" : "fail";
            fileRecord.reason = scanned ? describeEmptyFile (file) : scanError;
            add (fileRecord);
            std::printf ("    list: %s\n", fileRecord.reason.toRawUTF8());
            writeReport (reportFile, currentRecords(), filesDoneCount(), files.size(), false, startedAt);
            continue;
        }

        // 中のプラグインを 1 つずつ試す（途中で skip.txt に書かれたら、そこでやめる）
        bool skippedMidway = false;
        for (int j = 0; j < types.size(); ++j)
        {
            if (isSkipped (file))
            {
                skippedMidway = true;
                break;
            }

            const auto& d = types.getReference (j);
            Record r;
            r.type = "PLUGIN";
            r.key = fileKey + "|" + d.createIdentifierString().replaceCharacters ("|\t", "//");
            if (isDone (r.key))
                continue; // 前に試した

            r.file = file.getFullPathName();
            r.name = d.name;
            r.vendor = d.manufacturerName;
            r.version = d.version;
            r.category = d.category;
            r.instrument = d.isInstrument;

            std::printf ("    (%d/%d) %s ... ", j + 1, types.size(), d.name.toRawUTF8());
            std::fflush (stdout);

            tempOutput.deleteFile();
            bool finished = false, timedOut = false;
            int exitCode = -1;
            const auto start = nowMs();
            if (auto xml = d.createXml(); xml != nullptr && xml->writeTo (tempDescription))
            {
                const auto check = runChild ({ self.getFullPathName(), "--check", tempDescription.getFullPathName(), tempOutput.getFullPathName() },
                                             checkTimeoutMs);
                timedOut = check.timedOut;
                exitCode = check.exitCode;
                finished = check.started && ! check.timedOut && (exitCode == 0 || exitCode == 1);
            }
            r.seconds = (nowMs() - start) / 1000.0;

            const auto checkXml = juce::parseXMLIfTagMatches (tempOutput, "CHECK");
            judge (r, checkXml.get(), finished, timedOut, exitCode);
            add (r);
            std::printf ("%s %s\n", r.status.toRawUTF8(), r.reason.toRawUTF8());
            std::fflush (stdout);
            writeReport (reportFile, currentRecords(), filesDoneCount(), files.size(), false, startedAt);
        }

        if (skippedMidway)
        {
            recordSkip (file, fileKey);
            continue;
        }

        fileRecord.status = "ok";
        fileRecord.reason = juce::String (types.size());
        add (fileRecord);
    }

    tempDescription.deleteFile();
    tempOutput.deleteFile();
    workFolder.getChildFile ("list.xml").deleteFile();
    writeReport (reportFile, currentRecords(), filesDoneCount(), files.size(), true, startedAt);
    SetConsoleTitleW (L"VST3 check: done");
    std::printf ("\nDone. Results: %s\n", reportFile.getFullPathName().toRawUTF8());
    std::fflush (stdout);
    return 0;
}
} // namespace PluginCheck
