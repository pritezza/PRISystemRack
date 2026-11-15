#include "PluginScanner.h"

#include "HostCore/PluginBinaryCheck.h"

namespace
{
juce::File cacheFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("PRISystemRack")
        .getChildFile ("PluginCache.xml");
}

// 読み込んだ保存結果（メッセージスレッドだけで使う）。初めて使うときにファイルから読む
juce::XmlElement& cache()
{
    static std::unique_ptr<juce::XmlElement> loaded;
    if (loaded == nullptr)
    {
        loaded = juce::parseXMLIfTagMatches (cacheFile(), "PLUGINCACHE");
        if (loaded == nullptr)
            loaded = std::make_unique<juce::XmlElement> ("PLUGINCACHE");
    }
    return *loaded;
}

// ファイルが変わったかの目印（大きさと更新日時）
juce::String fileStamp (const juce::File& file)
{
    return juce::String (file.getSize()) + ":" + juce::String (file.getLastModificationTime().toMilliseconds());
}

juce::XmlElement* findEntry (const juce::File& file)
{
    for (auto* entry : cache().getChildWithTagNameIterator ("FILE"))
        if (entry->getStringAttribute ("path").equalsIgnoreCase (file.getFullPathName()))
            return entry;
    return nullptr;
}

void storeInCache (const juce::File& file, const juce::Array<juce::PluginDescription>& types)
{
    if (auto* old = findEntry (file))
        cache().removeChildElement (old, true);

    auto* entry = cache().createNewChildElement ("FILE");
    entry->setAttribute ("path", file.getFullPathName());
    entry->setAttribute ("stamp", fileStamp (file));
    for (const auto& type : types)
        entry->addChildElement (type.createXml().release());

    cacheFile().getParentDirectory().createDirectory();
    cache().writeTo (cacheFile());
}

// 調べられなかったことを、理由と一緒に記録する（中身なし、failed="1"）
void storeFailure (const juce::File& file, const PluginScanner::Failure& failure)
{
    if (auto* old = findEntry (file))
        cache().removeChildElement (old, true);

    auto* entry = cache().createNewChildElement ("FILE");
    entry->setAttribute ("path", file.getFullPathName());
    entry->setAttribute ("stamp", fileStamp (file));
    entry->setAttribute ("failed", true);
    entry->setAttribute ("reason", failure.reason);
    if (failure.a.isNotEmpty())
        entry->setAttribute ("a", failure.a);
    if (failure.b.isNotEmpty())
        entry->setAttribute ("b", failure.b);
    if (failure.runtimeNeed.isNotEmpty())
    {
        entry->setAttribute ("runtimeNeed", failure.runtimeNeed);
        entry->setAttribute ("runtimeHave", failure.runtimeHave);
    }

    cacheFile().getParentDirectory().createDirectory();
    cache().writeTo (cacheFile());
}

PluginScanner::Failure toFailure (const pri::host::PluginBinaryProblem& problem)
{
    using Kind = pri::host::PluginBinaryProblem::Kind;
    const juce::String a (problem.detail.c_str()), b (problem.detail2.c_str());
    switch (problem.kind)
    {
        case Kind::notX64:         return { "notX64", a, {} };
        case Kind::runtimeMissing: return { "runtimeMissing", a, {} };
        case Kind::runtimeTooOld:  return { "runtimeTooOld", a, b };
        case Kind::missingDll:     return { "missingDll", a, {} };
        case Kind::none:           break;
    }
    return {};
}

// 補助プログラムが落ちたときの終了コード（例外のコード）の説明
juce::String describeCrashCode (juce::uint32 code)
{
    switch (code)
    {
        case 0xC0000005: return TRANS ("access violation");
        case 0x40000015: return TRANS ("the plugin stopped itself (abort)");
        case 0xC0000374: return TRANS ("heap corruption");
        case 0xC0000409: return TRANS ("security check failure");
        case 0xC00000FD: return TRANS ("stack overflow");
        case 0xC0000094: return TRANS ("division by zero");
        case 0xE06D7363: return TRANS ("unhandled C++ exception");
        case 0xC000001D: return TRANS ("illegal instruction. The plugin may need a newer CPU (for example, one that supports AVX2)");
        case 0xC0000135: return TRANS ("a DLL that the plugin needs was not found");
        case 0xC0000139: return TRANS ("a function was not found in a DLL that the plugin needs. The Visual C++ runtime may be old");
        default: break;
    }
    return juce::String (pri::host::DescribeWindowsError (code).c_str());
}
} // namespace

juce::String PluginScanner::describeExceptionCode (juce::uint32 code)
{
    return describeCrashCode (code);
}

juce::String PluginScanner::describeFailure (const juce::File& file)
{
    const auto* entry = findEntry (file);
    if (entry == nullptr || ! entry->getBoolAttribute ("failed"))
        return {};

    auto text = describeReason (*entry);

    // 調べる前にランタイムが古そうだと分かっていた: 原因の候補として添える
    if (const auto need = entry->getStringAttribute ("runtimeNeed"); need.isNotEmpty())
        text << " " << TRANS ("The Visual C++ runtime on this PC (%have%) is older than the one this plugin was built with (%need%), which may be the cause.")
                           .replace ("%have%", entry->getStringAttribute ("runtimeHave"))
                           .replace ("%need%", need)
             << " " << TRANS ("Install the latest Microsoft Visual C++ Redistributable (x64), then press \"Rebuild list\".");
    return text;
}

juce::String PluginScanner::describeReason (const juce::XmlElement& entryRef)
{
    const auto* entry = &entryRef;
    const auto reason = entry->getStringAttribute ("reason");
    const auto a = entry->getStringAttribute ("a");
    const auto b = entry->getStringAttribute ("b");
    const auto installRedist = TRANS ("Install the latest Microsoft Visual C++ Redistributable (x64), then press \"Rebuild list\".");

    if (reason == "notX64")
        return TRANS ("This is not a 64-bit (x64) plugin (%machine%), so it cannot be used.").replace ("%machine%", a);
    if (reason == "runtimeMissing")
        return TRANS ("The Visual C++ runtime that this plugin needs (%dll%) is not installed on this PC.").replace ("%dll%", a) + " " + installRedist;
    if (reason == "runtimeTooOld")
        return TRANS ("The Visual C++ runtime on this PC (%have%) is older than the one this plugin needs (%need% or later).")
                   .replace ("%have%", b).replace ("%need%", a)
               + " " + installRedist;
    if (reason == "missingDll")
        return TRANS ("A DLL that this plugin needs was not found: %dll%").replace ("%dll%", a);
    if (reason == "loadError")
    {
        const auto message = juce::String (pri::host::DescribeWindowsError ((juce::uint32) a.getLargeIntValue()).c_str());
        auto text = TRANS ("Windows could not load this plugin (error %code%).").replace ("%code%", a) + (message.isNotEmpty() ? " " + message : juce::String());
        if (b.isNotEmpty()) // 見つからなかった DLL の候補（b）
            text << " " << TRANS ("DLLs that may be missing: %dll%").replace ("%dll%", b);
        return text;
    }
    if (reason == "exitCode")
        return TRANS ("The scanner ended with an error (exit code %code%).").replace ("%code%", a);
    if (reason == "crash")
    {
        if (a.isEmpty())
            return TRANS ("The scan ended without a result (the plugin may have crashed while being scanned).");
        const auto code = (juce::uint32) a.getHexValue64();
        const auto name = describeCrashCode (code);
        return TRANS ("The plugin crashed while being scanned (%code%).").replace ("%code%", "0x" + a.toUpperCase() + (name.isNotEmpty() ? ": " + name : juce::String()));
    }
    if (reason == "timeout")
        return TRANS ("Scanning took longer than %seconds% seconds and was stopped.").replace ("%seconds%", juce::String ((int) timeoutSeconds));
    return TRANS ("The reason was not recorded (it was scanned by an older version). \"Rebuild list\" scans it again.");
}

//==============================================================================
PluginScanner::~PluginScanner()
{
    cancel();
}

bool PluginScanner::findCached (const juce::File& file, juce::Array<juce::PluginDescription>& types)
{
    const auto* entry = findEntry (file);
    if (entry == nullptr || entry->getStringAttribute ("stamp") != fileStamp (file) || entry->getBoolAttribute ("failed"))
        return false;

    types.clear();
    for (auto* child : entry->getChildIterator())
    {
        juce::PluginDescription type;
        if (type.loadFromXml (*child))
            types.add (type);
    }
    return true;
}

bool PluginScanner::isKnownFailure (const juce::File& file)
{
    // 理由のない記録（理由を残すようになる前の版のもの）は、もう一度調べて理由を残す（そのときだけ調べ直す）
    const auto* entry = findEntry (file);
    // missingDll はファイルの場所だけで決めていた以前の判定（WinSxS の DLL を誤って見つからないとしていた）なので、これも調べ直す
    return entry != nullptr && entry->getBoolAttribute ("failed") && entry->hasAttribute ("reason")
           && entry->getStringAttribute ("reason") != "missingDll" && entry->getStringAttribute ("stamp") == fileStamp (file);
}

void PluginScanner::start (const juce::File& file)
{
    cancel();
    scanningFile = file;
    startMs = juce::Time::getMillisecondCounterHiRes();
    results.clear();
    errorText.clear();

    runtimeNeed.clear();
    runtimeHave.clear();

    // フォルダー形式で一覧の情報（moduleinfo.json）があるものは、プラグインを作らずにそれを読むだけなので、このプロセスで調べる
    const bool hasModuleInfo = file.getChildFile ("Contents").getChildFile ("Resources").getChildFile ("moduleinfo.json").existsAsFile();
    const auto scannerExe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("PRISystemRackScanner.exe");

    // ファイルの中身だけを見て、この PC で読み込めないと分かるもの（64 ビット用でない、ランタイムがない）は、読み込ませずに理由を記録する
    // （補助プログラムが落ちるのを待たない。このプロセスの中で調べる場合に本体が落ちるのも防ぐ）。
    // 「ランタイムが古い」はリンカーの版で決めていて、実際には動くこともあるので、補助プログラムで調べられるときは止めずに試し、
    // 失敗したときに原因の候補として添える
    const auto problem = pri::host::CheckPluginBinary (file.getFullPathName().toWideCharPointer());
    const bool helperCanTry = ! hasModuleInfo && scannerExe.existsAsFile();
    if (problem.kind == pri::host::PluginBinaryProblem::Kind::runtimeTooOld && helperCanTry)
    {
        runtimeNeed = juce::String (problem.detail.c_str());
        runtimeHave = juce::String (problem.detail2.c_str());
    }
    else if (const auto failure = toFailure (problem); failure.reason.isNotEmpty())
    {
        finishWith (State::failed, failure);
        return;
    }

    if (! hasModuleInfo && scannerExe.existsAsFile())
    {
        outputFile = juce::File::createTempFile (".xml");
        process = std::make_unique<juce::ChildProcess>();
        if (process->start (juce::StringArray { scannerExe.getFullPathName(), file.getFullPathName(), outputFile.getFullPathName() }, 0))
        {
            state = State::scanning;
            return;
        }
        process.reset();
    }

    // 一覧の情報があるとき、または補助プログラムが見つからない・起動できないときは、このプロセスの中で調べる
    // （後者は、その間は画面が止まる）
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, file.getFullPathName());
    for (auto* type : types)
        results.add (*type);
    storeInCache (file, results);
    state = State::done;
}

PluginScanner::State PluginScanner::poll (juce::Array<juce::PluginDescription>& types, juce::String& error)
{
    if (state == State::scanning && process != nullptr)
    {
        if (process->isRunning())
        {
            if (getElapsedSeconds() > timeoutSeconds)
            {
                process->kill();
                finishWith (State::failed, { "timeout", {}, {} });
            }
        }
        else
        {
            const auto exitCode = process->getExitCode();
            const auto xml = juce::parseXMLIfTagMatches (outputFile, "PLUGINS");

            if (xml != nullptr && xml->hasAttribute ("loadError"))
            {
                // 補助プログラムが Windows に DLL を読み込ませて失敗した（エラー番号）
                finishWith (State::failed, { "loadError", xml->getStringAttribute ("loadError"), xml->getStringAttribute ("missing") });
            }
            else if (exitCode != 0 && exitCode < 0x100)
            {
                // 落ちたのではなく、補助プログラムが失敗として終わった（結果のファイルを書けなかったなど）。例外のコードは大きい値
                finishWith (State::failed, { "exitCode", juce::String ((int) exitCode), {} });
            }
            else if (exitCode != 0 || xml == nullptr)
            {
                // 落ちた（終了コードは例外のコード。0 なのに結果がないときは不明）
                finishWith (State::failed, { "crash", exitCode != 0 ? juce::String::toHexString ((juce::int64) exitCode).paddedLeft ('0', 8) : juce::String(), {} });
            }
            else
            {
                for (auto* child : xml->getChildIterator())
                {
                    juce::PluginDescription type;
                    if (type.loadFromXml (*child))
                        results.add (type);
                }
                storeInCache (scanningFile, results);
                finishWith (State::done);
            }
        }
    }

    const auto current = state;
    if (current == State::done || current == State::failed)
    {
        types = results;
        error = errorText;
        state = State::idle;
    }
    return current;
}

void PluginScanner::finishWith (State newState, const Failure& failure)
{
    if (newState == State::failed)
    {
        auto withHint = failure;
        withHint.runtimeNeed = runtimeNeed;
        withHint.runtimeHave = runtimeHave;
        storeFailure (scanningFile, withHint);
    }

    process.reset();
    outputFile.deleteFile();
    errorText = newState == State::failed ? describeFailure (scanningFile) : juce::String();
    state = newState;
}

void PluginScanner::cancel()
{
    if (process != nullptr && process->isRunning())
        process->kill();
    process.reset();
    if (outputFile != juce::File())
        outputFile.deleteFile();
    state = State::idle;
}

double PluginScanner::getElapsedSeconds() const
{
    return (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
}
