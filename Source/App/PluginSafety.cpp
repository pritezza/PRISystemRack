#include "PluginSafety.h"

#include "HostCore/PluginBinaryCheck.h"
#include "PluginScanner.h"

#include <iterator>
#include <map>

namespace
{
bool enabled = true;

juce::String keyOf (const juce::PluginDescription& description)
{
    return description.fileOrIdentifier.toLowerCase() + "|"
         + (description.uniqueId != 0 ? juce::String::toHexString (description.uniqueId) : description.name);
}

// 残した判定の版。チェックの仕組みや、プラグインの作り方（JUCE への修正など）が変わって、前の判定が当てにならなくなったら上げる
// （違う版の判定は読まずに捨て、次に足すときにもう一度試す）。
// 2: JUCE が MIDI の入力のないプラグインにも CC の割り当てを 2,080 回問い合わせていたのを止めた（Waves 9.92 / 10.0 が使えるようになった）
// 3: MIDI の入力があるプラグインにも問い合わせないようにした（GTR Stomp 6 が使えるようになった）
constexpr int verdictVersion = 3;

// 読み込めないと分かった判定を残すファイル（%APPDATA%\PRISystemRack\PluginSafety.xml）
juce::File verdictFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("PRISystemRack")
        .getChildFile ("PluginSafety.xml");
}

// プラグインのファイルが変わったかの目印（実際の DLL の大きさと更新日時。フォルダー形式なら中の DLL）。
// 変わったら（プラグインを更新したら）、残した判定は使わずにもう一度試す
juce::String fileStampOf (const juce::String& fileOrIdentifier)
{
    const auto binary = pri::host::ResolvePluginBinary (fileOrIdentifier.toWideCharPointer());
    const juce::File file (binary.empty() ? fileOrIdentifier : juce::String (binary.c_str()));
    if (! file.existsAsFile())
        return {};
    return juce::String (file.getSize()) + ":" + juce::String (file.getLastModificationTime().toMilliseconds());
}

const char* const verdictAttributes[] = { "reason", "stage", "a", "b", "c", "runtimeNeed", "runtimeHave" };

juce::String& verdictField (PluginSafety::Verdict& v, int index)
{
    juce::String* fields[] = { &v.reason, &v.stage, &v.a, &v.b, &v.c, &v.runtimeNeed, &v.runtimeHave };
    return *fields[index];
}

// ファイルに残す判定か。プラグインのファイルが変わらない限り、何度試しても同じになるものだけ残す（64 ビット用でない、
// 描画資源を使い切る）。時間切れ・落ちた・結果なし・ランタイムや DLL がない・作れないは、PC の負荷やランタイムの入れ直し、
// ライセンスの状態などで変わることがあるので、起動している間だけ覚える（残すと、直してもチェックし直すまで読み込めないまま）
bool isLasting (const PluginSafety::Verdict& verdict)
{
    return verdict.blocked
        && ((verdict.reason == "binary" && verdict.a == "notX64") || verdict.reason == "tooManyGdi" || verdict.reason == "tooManyUser");
}

// 試した結果。キーはファイル + プラグインの ID か名前。読み込めないと分かったものはファイルにも残し（起動し直しても一覧に印を出す）、
// 初めて使うときにファイルから読む（プラグインのファイルが変わっていたものは捨てる）。問題なかったものは起動している間だけ覚える
std::map<juce::String, PluginSafety::Verdict>& results()
{
    static std::map<juce::String, PluginSafety::Verdict> map;
    static bool loaded = false;
    if (! loaded)
    {
        loaded = true;
        if (const auto xml = juce::parseXMLIfTagMatches (verdictFile(), "PLUGINSAFETY"); xml != nullptr && xml->getIntAttribute ("version") == verdictVersion)
            for (auto* entry : xml->getChildWithTagNameIterator ("VERDICT"))
            {
                const auto file = entry->getStringAttribute ("file");
                const auto stamp = entry->getStringAttribute ("stamp");
                if (file.isEmpty() || stamp.isEmpty() || fileStampOf (file) != stamp)
                    continue; // プラグインが更新された・なくなった

                PluginSafety::Verdict verdict;
                verdict.blocked = true;
                for (int i = 0; i < (int) std::size (verdictAttributes); ++i)
                    verdictField (verdict, i) = entry->getStringAttribute (verdictAttributes[i]);
                verdict.gdi = entry->getIntAttribute ("gdi");
                verdict.user = entry->getIntAttribute ("user");
                verdict.file = file;
                verdict.fileStamp = stamp;
                if (isLasting (verdict)) // 前の版が残した、変わることがある判定は使わない
                    map[entry->getStringAttribute ("key")] = verdict;
            }
    }
    return map;
}

// 読み込めないと分かった判定をファイルに書き直す
void saveVerdicts()
{
    juce::XmlElement root ("PLUGINSAFETY");
    root.setAttribute ("version", verdictVersion);
    for (const auto& [key, verdict] : results())
    {
        if (! isLasting (verdict) || verdict.fileStamp.isEmpty())
            continue;
        auto* entry = root.createNewChildElement ("VERDICT");
        entry->setAttribute ("key", key);
        entry->setAttribute ("file", verdict.file);
        entry->setAttribute ("stamp", verdict.fileStamp);
        for (int i = 0; i < (int) std::size (verdictAttributes); ++i)
            if (const auto& value = verdictField (const_cast<PluginSafety::Verdict&> (verdict), i); value.isNotEmpty())
                entry->setAttribute (verdictAttributes[i], value);
        entry->setAttribute ("gdi", verdict.gdi);
        entry->setAttribute ("user", verdict.user);
    }
    verdictFile().getParentDirectory().createDirectory();
    root.writeTo (verdictFile());
}

juce::File helperExe()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("PRISystemRackScanner.exe");
}
} // namespace

namespace PluginSafety
{
void setEnabled (bool shouldBeEnabled)
{
    enabled = shouldBeEnabled;
}

bool isEnabled()
{
    return enabled;
}

bool isBlocked (const juce::PluginDescription& description)
{
    if (! enabled)
        return false;
    const auto it = results().find (keyOf (description));
    return it != results().end() && it->second.blocked;
}

const Verdict* findVerdict (const juce::PluginDescription& description)
{
    const auto it = results().find (keyOf (description));
    return it != results().end() ? &it->second : nullptr;
}

void forget (const juce::PluginDescription& description)
{
    if (results().erase (keyOf (description)) > 0)
        saveVerdicts();
}

juce::String stageName (const juce::String& stage)
{
    if (stage == "binary")  return TRANS ("File contents (64-bit, Visual C++ runtime)");
    if (stage == "load")    return TRANS ("Loading the DLL");
    if (stage == "create")  return TRANS ("Creating the plugin");
    if (stage == "prepare") return TRANS ("Preparing (prepareToPlay)");
    if (stage == "process") return TRANS ("Processing silence");
    if (stage == "state")   return TRANS ("Saving and restoring the settings");
    return stage;
}

juce::String describe (const Verdict& verdict, const juce::String& pluginName)
{
    const auto& r = verdict.reason;
    const auto installRedist = TRANS ("Install the latest Microsoft Visual C++ Redistributable (x64), then try again.");

    if (r == "tooManyGdi")
        return TRANS ("%name% is not loaded: loading it uses up Windows drawing resources (%count% GDI objects for one instance; "
                      "an application can use up to 10,000), after which the application cannot draw its windows and crashes.")
            .replace ("%name%", pluginName)
            .replace ("%count%", juce::String (verdict.gdi));
    if (r == "tooManyUser")
        return TRANS ("%name% is not loaded: loading it uses up Windows window resources (%count% USER objects for one instance; "
                      "an application can use up to 10,000), after which the application cannot create its windows and crashes.")
            .replace ("%name%", pluginName)
            .replace ("%count%", juce::String (verdict.user));

    juce::String why;
    if (r == "binary")
    {
        if (verdict.a == "notX64")
            why = TRANS ("This is not a 64-bit (x64) plugin (%machine%), so it cannot be used.").replace ("%machine%", verdict.b);
        else if (verdict.a == "runtimeMissing")
            why = TRANS ("The Visual C++ runtime that this plugin needs (%dll%) is not installed on this PC.").replace ("%dll%", verdict.b)
                  + " " + installRedist;
        else
            why = TRANS ("The Visual C++ runtime on this PC (%have%) is older than the one this plugin needs (%need% or later).")
                      .replace ("%have%", verdict.c).replace ("%need%", verdict.b)
                  + " " + installRedist;
    }
    else if (r == "loadError")
    {
        const auto message = juce::String (pri::host::DescribeWindowsError ((juce::uint32) verdict.a.getLargeIntValue()).c_str());
        why = TRANS ("Windows could not load this plugin (error %code%).").replace ("%code%", verdict.a)
              + (message.isNotEmpty() ? " " + message : juce::String());
        if (verdict.b.isNotEmpty())
            why << " " << TRANS ("DLLs that may be missing: %dll%").replace ("%dll%", verdict.b);
    }
    else if (r == "createFailed")
    {
        why = TRANS ("The plugin could not be created.") + (verdict.a.isNotEmpty() ? " (" + verdict.a + ")" : juce::String());
    }
    else if (r == "crashed")
    {
        const auto name = PluginScanner::describeExceptionCode ((juce::uint32) verdict.a.getHexValue64());
        why = TRANS ("The plugin crashed during the check (%stage%: %code%).")
                  .replace ("%stage%", stageName (verdict.stage))
                  .replace ("%code%", "0x" + verdict.a.toUpperCase() + (name.isNotEmpty() ? ", " + name : juce::String()));
    }
    else if (r == "timeout")
    {
        why = TRANS ("The check took longer than %seconds% seconds and was stopped (%stage%).")
                  .replace ("%seconds%", juce::String ((int) timeoutSeconds))
                  .replace ("%stage%", stageName (verdict.stage));
    }
    else
    {
        why = TRANS ("The check ended without a result (%stage%).").replace ("%stage%", stageName (verdict.stage));
    }

    // ファイルの中身から、ランタイムが古そうだと分かっていた: 原因の候補として添える
    if (verdict.runtimeNeed.isNotEmpty())
        why << " " << TRANS ("The Visual C++ runtime on this PC (%have%) is older than the one this plugin was built with (%need%), which may be the cause.")
                          .replace ("%have%", verdict.runtimeHave)
                          .replace ("%need%", verdict.runtimeNeed)
            << " " << installRedist;

    return TRANS ("%name% is not loaded.").replace ("%name%", pluginName) + " " + why;
}

juce::String describeShort (const Verdict& verdict)
{
    const auto& r = verdict.reason;
    if (r == "tooManyGdi" || r == "tooManyUser") return TRANS ("uses up drawing resources");
    if (r == "binary")                          return verdict.a == "notX64" ? TRANS ("not 64-bit") : TRANS ("Visual C++ runtime");
    if (r == "loadError")                       return TRANS ("Windows could not load it");
    if (r == "createFailed")                    return TRANS ("could not be created");
    if (r == "crashed")                         return TRANS ("crashed");
    if (r == "timeout")                         return TRANS ("timed out");
    return TRANS ("no result");
}

//==============================================================================
Probe::~Probe()
{
    cancel();
}

bool Probe::start (const juce::PluginDescription& descriptionToTry)
{
    cancel();
    description = descriptionToTry;
    last = {};

    const auto helper = helperExe();
    if (! helper.existsAsFile())
        return false;

    descriptionFile = juce::File::createTempFile (".xml");
    outputFile = juce::File::createTempFile (".xml");
    if (auto xml = description.createXml(); xml == nullptr || ! xml->writeTo (descriptionFile))
    {
        cleanUp();
        return false;
    }

    process = std::make_unique<juce::ChildProcess>();
    if (! process->start (juce::StringArray { helper.getFullPathName(), "--probe", descriptionFile.getFullPathName(), outputFile.getFullPathName(),
                                              juce::String (gdiThreshold), juce::String (userThreshold) },
                          0))
    {
        cleanUp();
        return false;
    }
    startMs = juce::Time::getMillisecondCounterHiRes();
    return true;
}

bool Probe::poll (Progress& progress, Verdict& verdict)
{
    if (process == nullptr)
        return false;

    // 先に終わったかを見てから、補助プログラムが書いた進み具合を読む（逆の順だと、結果を書いて終わる直前の古い内容を読み、
    // 終わったのに結果がない（noResult）と誤って判定することがある。終わったあとに読めば、最後に書いた内容が必ず読める）。
    // 書き換えの途中で読めないときは前の値のまま
    const bool running = process->isRunning();
    const auto report = juce::parseXMLIfTagMatches (outputFile, "PROBE");
    if (report != nullptr)
    {
        last.stage = report->getStringAttribute ("stage", last.stage);
        last.gdi = report->getIntAttribute ("gdi", last.gdi);
        last.user = report->getIntAttribute ("user", last.user);
    }
    last.seconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
    progress = last;

    if (running && last.seconds < timeoutSeconds)
        return false;

    verdict = {};
    verdict.stage = last.stage;
    verdict.gdi = juce::jmax (0, last.gdi);
    verdict.user = juce::jmax (0, last.user);
    if (report != nullptr)
    {
        verdict.runtimeNeed = report->getStringAttribute ("runtimeNeed");
        verdict.runtimeHave = report->getStringAttribute ("runtimeHave");
    }

    if (running)
    {
        process->kill();
        verdict.reason = "timeout";
    }
    else
    {
        const auto exitCode = process->getExitCode();
        const auto result = report != nullptr ? report->getStringAttribute ("result") : juce::String();
        if (result.isNotEmpty())
        {
            verdict.reason = result;
            verdict.a = report->getStringAttribute ("a");
            verdict.b = report->getStringAttribute ("b");
            verdict.c = report->getStringAttribute ("c");
            if (result == "tooManyGdi")
                verdict.gdi = verdict.a.getIntValue();
            if (result == "tooManyUser")
                verdict.user = verdict.a.getIntValue();
        }
        else if (exitCode >= 0x100) // 落ちた（終了コードは例外のコード）
        {
            verdict.reason = "crashed";
            verdict.a = juce::String::toHexString ((juce::int64) exitCode).paddedLeft ('0', 8);
        }
        else
        {
            verdict.reason = "noResult";
        }
    }

    verdict.blocked = verdict.reason != "ok";
    verdict.file = description.fileOrIdentifier;
    verdict.fileStamp = fileStampOf (description.fileOrIdentifier);
    auto& stored = results()[keyOf (description)];
    const bool wasLasting = isLasting (stored);
    stored = verdict;
    if (wasLasting || isLasting (verdict))
        saveVerdicts(); // 何度試しても同じになる判定はファイルにも残す（起動し直しても一覧に印を出し、試さずに理由を出す）。前に残したものは書き直して消す
    cleanUp();
    return true;
}

void Probe::cancel()
{
    if (process != nullptr && process->isRunning())
        process->kill();
    cleanUp();
}

void Probe::cleanUp()
{
    // 止めた補助プログラムが本当に終わるまで少し待つ（終わる前に消そうとすると、使用中で一時ファイルが消せずに残る）
    if (process != nullptr)
        process->waitForProcessToFinish (2000);
    process.reset();
    if (descriptionFile != juce::File())
        descriptionFile.deleteFile();
    if (outputFile != juce::File())
        outputFile.deleteFile();
    descriptionFile = outputFile = juce::File();
}
} // namespace PluginSafety
