// PRISystemRackScanner: VST3 ファイルの中のプラグインの一覧を調べて XML に書く（本体アプリが別のプロセスとして起動する）。
//
//   PRISystemRackScanner.exe <plugin.vst3> <output.xml>
//   PRISystemRackScanner.exe --probe <description.xml> <output.xml> [<GDI の上限> <USER の上限>]（プラグイン安全チェック）
//   PRISystemRackScanner.exe --probe-hold <description.xml> <output.xml> <seconds>（調査用: 作ったあと seconds 秒動かし続ける）
//   PRISystemRackScanner.exe --render-test <description.xml> <rate> <blockSize> [sine|noise|host,noise] [state.txt]（調査用: 音が壊れるかを測る）
//   PRISystemRackScanner.exe --analyze-f32 <file.f32> <rate>（調査用: 本体の --dump-taps が書いた入出力を調べる）
//   PRISystemRackScanner.exe --check <description.xml> <output.xml>（調査用: 1 つ作って音を通し、設定を戻せるかを試す）
//   PRISystemRackScanner.exe --check-all <作業フォルダー> <結果の .md>（調査用: このPCの VST3 をすべて --check で試す。PluginCheck.h）
//
// Waves の WaveShell のように 1 つのファイルに多数のプラグインが入っていると、JUCE は中のプラグインを一つずつ作って
// 情報を読むので 30 秒以上かかることがある。本体とは別のプロセスで調べるので、その間も本体の画面は止まらず、
// 調べている途中でプラグインが落ちても本体は落ちない。
// 出力: <PLUGINS> の下に PluginDescription::createXml() の要素を並べる。終了コード: 0 = 書けた（0 個のこともある）、
// 1 = 書けなかった、2 = 使い方の誤り
//
// --probe <description.xml> <output.xml> [<GDI の上限> <USER の上限>]: プラグイン安全チェック。本体に入れる前に、
// description.xml（PluginDescription::createXml()）のプラグインを、本体と同じ手順で 1 つ作って試す。段階は順に
//   binary（ファイルの中身: 64 ビット用か、Visual C++ ランタイム）→ load（Windows に DLL を読み込ませる）→ create（作る）→
//   prepare（prepareToPlay と無音 1 回）→ process（無音を 20 回）→ state（設定の保存と復元、もう一度無音）→ done
// 段階に入るたびと、0.25 秒ごとに <PROBE> を書き直す（stage、経過秒数、増えた GDI / USER オブジェクトの数）。本体はそれを読んで
// ダイアログに進み具合を出す。描画資源（GDI / USER オブジェクト）は別のスレッドで測り、上限を超えたらその場で打ち切る
// （WaveShell 10.0 の Vocal Rider や L3 は、作るだけで GDI を上限の 10,000 個まで使う。使い切るまで待つと 1 分ほどかかる。
// 使い切ると解放しても戻らないので、本体の中では試せない）。
// 読み込めないと分かったら result に理由（binary / loadError / createFailed / tooManyGdi / tooManyUser）を書いて終了コード 3、
// 最後まで通れば result="ok" で 0。途中で落ちたら、終了コード（例外のコード）と最後に書いた stage で本体が判断する

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginCheck.h"
#include "HostCore/PluginBinaryCheck.h"

#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#include <xmmintrin.h>
#include <dbghelp.h> // 調査用の呼び出し経路（dumpThreadStack）

#pragma comment(lib, "dbghelp.lib")

#include <atomic>
#include <thread>

namespace
{
// 安全チェックの進み具合と結果（<PROBE>）。本体が読むので、段階の切り替えと描画資源の測定（別のスレッド）のたびに書き直す
class ProbeReport
{
public:
    explicit ProbeReport (const juce::File& fileToWrite) : file (fileToWrite) {}

    void set (const juce::Identifier& name, const juce::var& value)
    {
        const juce::ScopedLock scope (lock);
        xml.setAttribute (name, value.toString());
    }

    void write()
    {
        const juce::ScopedLock scope (lock);
        xml.setAttribute ("seconds", (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0);
        xml.writeTo (file); // 一時ファイルに書いてから置き換えるので、本体が書きかけを読むことはない
    }

    void stage (const char* name)
    {
        set ("stage", name);
        write();
    }

    // 結果を書いて、プラグインの後始末を待たずに終わる（読み込めない理由なら終了コード 3、問題なしなら 0）。
    // 理由を書いてから終わるまでをロックの中で行う: 別のスレッド（描画資源の見張り）が同時に終えようとしても、先に入った方の
    // 理由だけが書かれ（属性が混ざらない）、書きかけの一時ファイルを残して終わることもない（書き込みも同じロックの中で行うため）
    [[noreturn]] void finish (const char* reason, const juce::String& a = {}, const juce::String& b = {}, const juce::String& c = {})
    {
        const juce::ScopedLock scope (lock);
        xml.setAttribute ("result", reason);
        if (a.isNotEmpty()) xml.setAttribute ("a", a);
        if (b.isNotEmpty()) xml.setAttribute ("b", b);
        if (c.isNotEmpty()) xml.setAttribute ("c", c);
        write();
        std::fflush (nullptr);
        TerminateProcess (GetCurrentProcess(), std::strcmp (reason, "ok") == 0 ? 0 : 3);
        for (;;) {}
    }

    [[noreturn]] void block (const char* reason, const juce::String& a = {}, const juce::String& b = {}, const juce::String& c = {})
    {
        finish (reason, a, b, c);
    }

private:
    const juce::File file;
    juce::CriticalSection lock;
    juce::XmlElement xml { "PROBE" };
    const double startMs = juce::Time::getMillisecondCounterHiRes();
};

// 本体（PluginChain::FixedPlayHead）と同じく、止まっている 120 BPM・4/4 を渡す（テンポを 0 として受け取ると落ちるプラグインがある）
class StoppedPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (120.0);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setIsPlaying (false);
        return info;
    }
};

bool setMainBusesTo (juce::AudioPluginInstance& plugin, const juce::AudioChannelSet& set)
{
    auto layout = plugin.getBusesLayout();
    if (layout.inputBuses.size() > 0)
        layout.inputBuses.getReference (0) = set;
    if (layout.outputBuses.size() > 0)
        layout.outputBuses.getReference (0) = set;
    return plugin.setBusesLayout (layout);
}

void processSilence (juce::AudioPluginInstance& instance, int blocks, int blockSize)
{
    juce::AudioBuffer<float> buffer (juce::jmax (2, instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels()), blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < blocks; ++i)
    {
        buffer.clear();
        midi.clear();
        const juce::ScopedLock callbackLock (instance.getCallbackLock());
        instance.processBlock (buffer, midi);
    }
}

// 調査用（--probe の 6 番目の引数に stack）: 別のスレッドから threadId のスレッドを一瞬止めて、呼び出し経路を out に書く。
// 止めている間はメモリーを確保しない（止めたスレッドがヒープのロックを持っていると止まるため）。RtlVirtualUnwind で
// 戻り先を集めてから再開し、そのあとで関数名に直す
void dumpThreadStack (DWORD threadId, FILE* out, const char* label)
{
    std::vector<DWORD64> addresses;
    addresses.reserve (96);

    if (const HANDLE thread = OpenThread (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId))
    {
        if (SuspendThread (thread) != (DWORD) -1)
        {
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext (thread, &context))
            {
                for (int i = 0; i < 96 && context.Rip != 0; ++i)
                {
                    addresses.push_back (context.Rip);
                    DWORD64 imageBase = 0;
                    if (const auto function = RtlLookupFunctionEntry (context.Rip, &imageBase, nullptr))
                    {
                        PVOID handlerData = nullptr;
                        DWORD64 establisherFrame = 0;
                        RtlVirtualUnwind (UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
                    }
                    else
                    {
                        context.Rip = *reinterpret_cast<DWORD64*> (context.Rsp);
                        context.Rsp += 8;
                    }
                }
            }
            ResumeThread (thread);
        }
        CloseHandle (thread);
    }

    const HANDLE process = GetCurrentProcess();
    static bool symbolsReady = false;
    if (! symbolsReady)
    {
        SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize (process, nullptr, TRUE);
        symbolsReady = true;
    }

    std::fprintf (out, "--- %s\n", label);
    alignas (SYMBOL_INFO) char symbolBuffer[sizeof (SYMBOL_INFO) + 512] {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*> (symbolBuffer);
    for (const auto address : addresses)
    {
        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        IMAGEHLP_MODULE64 module {};
        module.SizeOfStruct = sizeof (module);
        const char* moduleName = SymGetModuleInfo64 (process, address, &module) ? module.ModuleName : "?";
        if (SymFromAddr (process, address, &displacement, symbol))
            std::fprintf (out, "  %s!%s+0x%llx\n", moduleName, symbol->Name, (unsigned long long) displacement);
        else
            std::fprintf (out, "  %s!0x%llx\n", moduleName, (unsigned long long) address);
    }
    std::fflush (out);
}

// --probe: プラグイン安全チェック（上の説明）
int safetyProbe (const juce::File& descriptionFile, const juce::File& outputFile, int gdiLimit, int userLimit, bool dumpStacks = false)
{
    const DWORD mainThreadId = GetCurrentThreadId();
    FILE* stackOut = nullptr;
    if (dumpStacks)
        _wfopen_s (&stackOut, (outputFile.getFullPathName() + ".stack.txt").toWideCharPointer(), L"w");
    int nextStackAt = 300; // 調査用: GDI がこの数を超えるたびに呼び出し経路を書く（300, 1000, 3000）

    juce::PluginDescription description;
    const auto xml = juce::parseXML (descriptionFile);
    if (xml == nullptr || ! description.loadFromXml (*xml))
        return 2;

    ProbeReport report (outputFile);
    const auto process = GetCurrentProcess();
    const int gdiBefore = (int) GetGuiResources (process, GR_GDIOBJECTS);
    const int userBefore = (int) GetGuiResources (process, GR_USEROBJECTS);

    // 描画資源を 0.25 秒ごとに測り、上限を超えたらその場で打ち切る（プラグインを作っている最中でも測れるよう、別のスレッドで）。
    // 本体の時間切れ（120 秒）より少し長く経っても終わっていなければ、自分で終わる（本体が落ちて止めてもらえないときに残らないよう）
    std::atomic<bool> finished { false };
    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    std::thread watcher ([&]
    {
        while (! finished.load())
        {
            const int gdi = (int) GetGuiResources (process, GR_GDIOBJECTS) - gdiBefore;
            const int user = (int) GetGuiResources (process, GR_USEROBJECTS) - userBefore;
            report.set ("gdi", gdi);
            report.set ("user", user);
            if (stackOut != nullptr && gdi >= nextStackAt)
            {
                char label[64];
                std::snprintf (label, sizeof (label), "GDI %d", gdi);
                dumpThreadStack (mainThreadId, stackOut, label);
                nextStackAt = nextStackAt < 1000 ? 1000 : nextStackAt * 3;
            }
            if (gdi >= gdiLimit)
                report.block ("tooManyGdi", juce::String (gdi));
            if (user >= userLimit)
                report.block ("tooManyUser", juce::String (user));
            if (juce::Time::getMillisecondCounterHiRes() - startMs > 130000.0)
                report.block ("timeout");
            report.write();
            Sleep (250);
        }
    });
    watcher.detach(); // 終わるときは TerminateProcess で一緒に止まる

    // 1. ファイルの中身（実行しない）
    report.stage ("binary");
    const auto problem = pri::host::CheckPluginBinary (description.fileOrIdentifier.toWideCharPointer());
    switch (problem.kind)
    {
        case pri::host::PluginBinaryProblem::Kind::notX64:
            report.block ("binary", "notX64", juce::String (problem.detail.c_str()));
        case pri::host::PluginBinaryProblem::Kind::runtimeMissing:
            report.block ("binary", "runtimeMissing", juce::String (problem.detail.c_str()));
        case pri::host::PluginBinaryProblem::Kind::runtimeTooOld:
            // ランタイムの版の比べ方はリンカーの版に頼っていて、実際には動くこともある（古いツールでコンパイルし、新しいリンカーで
            // リンクしたものなど）。ここでは止めずに控えだけ書き、このあとの段階で失敗したときに、原因の候補として本体が添える
            report.set ("runtimeNeed", juce::String (problem.detail.c_str()));
            report.set ("runtimeHave", juce::String (problem.detail2.c_str()));
            break;
        default:
            break;
    }

    // 2. Windows に DLL を読み込ませる（読み込めなければエラー番号と、見つからない可能性のある DLL）
    report.stage ("load");
    if (const auto binary = pri::host::ResolvePluginBinary (description.fileOrIdentifier.toWideCharPointer()); ! binary.empty())
        if (LoadLibraryW (binary.c_str()) == nullptr)
        {
            const DWORD error = GetLastError();
            report.block ("loadError", juce::String ((juce::int64) error),
                          error == ERROR_MOD_NOT_FOUND ? juce::String (pri::host::FindUnresolvedImports (description.fileOrIdentifier.toWideCharPointer()).c_str())
                                                       : juce::String());
        }

    // 3. 作る（本体と同じ 48 kHz・480 サンプル）
    report.stage ("create");
    constexpr double rate = 48000.0;
    constexpr int blockSize = 480;
    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
    juce::String error;
    auto instance = formats.createPluginInstance (description, rate, blockSize, error);
    if (instance == nullptr)
        report.block ("createFailed", error);

    // 4. 準備（PluginChain::preparePlugin と同じ手順）と、準備の直後の無音 1 回
    report.stage ("prepare");
    static StoppedPlayHead playHead;
    instance->setPlayHead (&playHead);
    instance->releaseResources();
    if (! setMainBusesTo (*instance, juce::AudioChannelSet::stereo()))
        setMainBusesTo (*instance, juce::AudioChannelSet::mono());
    instance->prepareToPlay (rate, blockSize);
    processSilence (*instance, 1, blockSize);

    // 5. 無音を何回か処理する
    report.stage ("process");
    processSilence (*instance, 20, blockSize);

    // 6. 設定を保存して戻し、もう一度無音を処理する
    report.stage ("state");
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    if (state.getSize() > 0)
        instance->setStateInformation (state.getData(), (int) state.getSize());
    processSilence (*instance, 5, blockSize);

    // 最後にもう一度描画資源を測ってから、問題なしとする
    const int gdi = (int) GetGuiResources (process, GR_GDIOBJECTS) - gdiBefore;
    const int user = (int) GetGuiResources (process, GR_USEROBJECTS) - userBefore;
    if (gdi >= gdiLimit)
        report.block ("tooManyGdi", juce::String (gdi));
    if (user >= userLimit)
        report.block ("tooManyUser", juce::String (user));

    // 問題なし。プラグインの解放は待たずに終わる（古いプラグインは解放に時間がかかったり落ちたりする）
    finished = true;
    report.set ("gdi", gdi);
    report.set ("user", user);
    report.set ("stage", "done");
    report.finish ("ok");
}

// 調査用（--probe-hold）: 1 つ作って描画資源の増えた数を書き、そのあと holdSeconds 秒動かし続ける
int probe (const juce::File& descriptionFile, const juce::File& outputFile, int holdSeconds = 0)
{
    juce::PluginDescription description;
    const auto xml = juce::parseXML (descriptionFile);
    if (xml == nullptr || ! description.loadFromXml (*xml))
        return 2;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    const auto process = GetCurrentProcess();
    const auto gdiBefore = (int) GetGuiResources (process, GR_GDIOBJECTS);
    const auto userBefore = (int) GetGuiResources (process, GR_USEROBJECTS);
    const auto start = juce::Time::getMillisecondCounterHiRes();

    juce::String error;
    auto instance = formats.createPluginInstance (description, 48000.0, 480, error);

    juce::XmlElement result ("PROBE");
    result.setAttribute ("created", instance != nullptr);
    result.setAttribute ("error", error);
    result.setAttribute ("seconds", (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0);
    result.setAttribute ("gdi", (int) GetGuiResources (process, GR_GDIOBJECTS) - gdiBefore);
    result.setAttribute ("user", (int) GetGuiResources (process, GR_USEROBJECTS) - userBefore);

    const bool written = result.writeTo (outputFile);

    // 調査用（--probe-hold）: 本体と同じように準備して無音を処理しながら、メッセージを回して holdSeconds 秒置く。
    // 作ったあとでスレッドやメモリがどう増えるかを外から測るため
    if (holdSeconds > 0 && instance != nullptr)
    {
        instance->setPlayConfigDetails (2, 2, 48000.0, 480);
        instance->prepareToPlay (48000.0, 480);
        juce::AudioBuffer<float> buffer (juce::jmax (2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels()), 480);
        juce::MidiBuffer midi;
        const auto until = juce::Time::getMillisecondCounterHiRes() + holdSeconds * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < until)
        {
            MSG msg;
            while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&msg);
                DispatchMessageW (&msg);
            }
            buffer.clear();
            instance->processBlock (buffer, midi);
            Sleep (10);
        }
    }

    // 解放は待たない（古いプラグインは解放に時間がかかったり落ちたりすることがある。結果はもう書いた）
    instance.release();
    return written ? 0 : 1;
}
// 調査用（--render-test）: 440 Hz のサイン波（-12 dBFS）を blockSize ずつ渡して 3 秒処理し、後半 2 秒の出力について
// 全体の強さに対する 440 Hz の割合と、周期の速さ（rate / blockSize Hz）の成分の割合を出す。
// 周期の長さによって音が壊れるプラグイン（周期ごとのぶつ切れ・ノイズ）を見分けるため
double goertzelPower (const std::vector<float>& x, double frequency, double rate)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * frequency / rate;
    const double coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (const float v : x)
    {
        const double s = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / ((double) x.size() * x.size() / 4.0); // 振幅² ほど
}

int renderTest (const juce::File& descriptionFile, double rate, int blockSize, bool variable, bool likeHost, bool noise,
                const juce::File& stateFile)
{
    juce::PluginDescription description;
    const auto xml = juce::parseXML (descriptionFile);
    if (xml == nullptr || ! description.loadFromXml (*xml))
        return 2;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
    juce::String error;
    auto instance = formats.createPluginInstance (description, rate, blockSize, error);
    if (instance == nullptr)
    {
        std::printf ("cannot create: %s\n", error.toRawUTF8());
        return 1;
    }

    auto layout = instance->getBusesLayout();
    if (! layout.inputBuses.isEmpty())  layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    instance->setBusesLayout (layout);
    instance->prepareToPlay (rate, blockSize);

    // 本体に保存された設定（MemoryBlock::toBase64Encoding の文字列）があれば読み込ませる
    if (stateFile.existsAsFile())
    {
        juce::MemoryBlock state;
        if (state.fromBase64Encoding (stateFile.loadFileAsString().trim()))
        {
            instance->setStateInformation (state.getData(), (int) state.getSize());
            std::printf ("loaded state: %d bytes\n", (int) state.getSize());
        }
    }

    const int channels = juce::jmax (2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer (channels, blockSize);
    juce::MidiBuffer midi;

    // likeHost: 本体と同じく、いったん止めて準備し直し、無音を 1 回処理してから、画面を開いて別のスレッドで実時間で処理する
    // 本体と同じ再生位置の情報（120 BPM、4/4、止まっている。FixedPlayHead のまね）
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
    static StoppedPlayHead playHead;

    std::unique_ptr<juce::DocumentWindow> window;
    if (likeHost)
    {
        instance->setPlayHead (&playHead);
        instance->releaseResources();
        instance->prepareToPlay (rate, blockSize);
        buffer.clear();
        instance->processBlock (buffer, midi);

        if (auto* editor = instance->createEditorAndMakeActive())
        {
            window = std::make_unique<juce::DocumentWindow> ("render test", juce::Colours::black, juce::DocumentWindow::closeButton);
            window->setUsingNativeTitleBar (true);
            window->setContentNonOwned (editor, true);
            window->setTopLeftPosition (100, 100);
            window->setVisible (true);
        }
    }

    const int total = (int) (rate * 3.0), measureFrom = (int) rate;
    std::vector<float> output;
    output.reserve ((size_t) (total - measureFrom));

    double sumMs = 0.0, maxMs = 0.0;
    int calls = 0, overruns = 0; // overruns: 1 回の処理が周期の長さを超えた回数
    std::vector<float> input;
    juce::Random noiseRandom (42);
    int csrChanges = 0; // processBlock の前後で浮動小数点の設定（MXCSR）が変わった回数
    int guardReports = 0, guardHits = 0; // 渡した範囲の外に書き込んだ（報告した回数、チャンネル×回の数）

    // variable: 1 回ごとに 1〜blockSize のばらばらの長さで渡す（周期の長さが毎回変わるホストのまね）
    auto run = [&]
    {
        juce::Random random (1234);
        const auto start = juce::Time::getMillisecondCounterHiRes();
        for (int pos = 0, n = blockSize; pos < total; pos += n)
        {
            n = variable ? 1 + random.nextInt (blockSize) : blockSize;
            buffer.setSize (channels, n, false, false, true);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                if (noise) // 左右で違うノイズ（-20 dBFS ほど）
                {
                    const float l = 0.1f * (noiseRandom.nextFloat() * 2.0f - 1.0f) * 1.7f;
                    const float r = 0.1f * (noiseRandom.nextFloat() * 2.0f - 1.0f) * 1.7f;
                    buffer.setSample (0, i, l);
                    buffer.setSample (1, i, r);
                    if (pos + i >= measureFrom && pos + i < total)
                        input.push_back (l);
                }
                else
                {
                    const float v = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (pos + i) / rate);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                }
            }
            {
                const juce::ScopedLock lock (instance->getCallbackLock());
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                const unsigned int csrBefore = _mm_getcsr();

                // 渡す範囲の前後に見張りの領域を付けたバッファで処理させ、範囲の外に書き込んだかを調べる
                constexpr int guard = 4096;
                constexpr float magic = 12345.0f;
                std::vector<std::vector<float>> guarded ((size_t) channels, std::vector<float> ((size_t) (guard + n + guard), magic));
                std::vector<float*> pointers;
                for (int ch = 0; ch < channels; ++ch)
                {
                    std::copy (buffer.getReadPointer (ch), buffer.getReadPointer (ch) + n, guarded[(size_t) ch].begin() + guard);
                    pointers.push_back (guarded[(size_t) ch].data() + guard);
                }
                juce::AudioBuffer<float> view (pointers.data(), channels, n);
                instance->processBlock (view, midi);
                for (int ch = 0; ch < channels; ++ch)
                {
                    std::copy (pointers[(size_t) ch], pointers[(size_t) ch] + n, buffer.getWritePointer (ch));
                    const auto& g = guarded[(size_t) ch];
                    int before = 0, after = 0, lastAfter = -1;
                    for (int i = 0; i < guard; ++i)
                        if (g[(size_t) i] != magic)
                            ++before;
                    for (int i = 0; i < guard; ++i)
                        if (g[(size_t) (guard + n + i)] != magic)
                        {
                            ++after;
                            lastAfter = i;
                        }
                    if ((before > 0 || after > 0) && guardReports++ < 6)
                        std::printf ("  OUT OF RANGE WRITE: block %d (n=%d) channel %d: %d samples before, %d samples after (up to +%d)\n",
                                     calls, n, ch, before, after, lastAfter + 1);
                    if (before > 0 || after > 0)
                        ++guardHits;
                }

                const unsigned int csrAfter = _mm_getcsr();
                if (csrBefore != csrAfter && csrChanges++ < 3)
                    std::printf ("  MXCSR changed by processBlock: 0x%08X -> 0x%08X\n", csrBefore, csrAfter);
                const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
                sumMs += ms;
                maxMs = juce::jmax (maxMs, ms);
                ++calls;
                if (ms > 1000.0 * n / rate)
                    ++overruns;
            }
            for (int i = 0; i < n; ++i)
                if (pos + i >= measureFrom && pos + i < total)
                    output.push_back (buffer.getSample (0, i));

            if (likeHost) // 実時間に合わせる
                while (juce::Time::getMillisecondCounterHiRes() - start < 1000.0 * (pos + n) / rate)
                    Sleep (1);
        }
    };

    if (likeHost)
    {
        std::atomic<bool> done { false };
        std::thread audio ([&] { run(); done = true; });
        while (! done)
        {
            MSG msg;
            while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&msg);
                DispatchMessageW (&msg);
            }
            Sleep (5);
        }
        audio.join();
    }
    else
    {
        run();
    }

    double energy = 0.0;
    for (const float v : output)
        energy += (double) v * v;
    energy = energy / (double) output.size() * 2.0; // 振幅² ほど（サイン波なら振幅²）

    const double tone = goertzelPower (output, 440.0, rate);
    double blockRate = 0.0;
    for (int k = 1; k <= 5; ++k)
        blockRate += goertzelPower (output, k * rate / blockSize, rate);

    std::printf (likeHost ? "[like host] " : "");
    std::printf ("rate %.0f block %d%s: rms %.4f  440Hz %.1f%%  block-rate harmonics (%.1f Hz x1..5) %.2f%%\n", rate, blockSize,
                 variable ? " (variable)" : "", std::sqrt (energy / 2.0), energy > 0 ? 100.0 * tone / energy : 0.0, rate / blockSize, energy > 0 ? 100.0 * blockRate / energy : 0.0);
    std::printf ("  processBlock: avg %.3f ms  max %.3f ms  over the period %d / %d  (latency %d samples)\n",
                 calls > 0 ? sumMs / calls : 0.0, maxMs, overruns, calls, instance->getLatencySamples());
    std::printf ("  out-of-range writes: %d (channel x block)\n", guardHits);

    // ノイズのとき: 出力の 20〜4000 Hz を 2 Hz おきに調べ、入力にない一定の高さの音（周りより強い山）を探す
    if (noise && ! input.empty())
    {
        const std::vector<float> outTail (output.end() - (std::ptrdiff_t) juce::jmin ((size_t) rate, output.size()), output.end());
        const std::vector<float> inTail (input.end() - (std::ptrdiff_t) juce::jmin ((size_t) rate, input.size()), input.end());
        std::vector<std::pair<double, double>> outPower, inPower; // (Hz, 強さ)
        for (double hz = 20.0; hz <= 4000.0; hz += 2.0)
        {
            outPower.emplace_back (hz, goertzelPower (outTail, hz, rate));
            inPower.emplace_back (hz, goertzelPower (inTail, hz, rate));
        }
        auto median = [] (std::vector<std::pair<double, double>> v)
        {
            std::nth_element (v.begin(), v.begin() + (std::ptrdiff_t) (v.size() / 2), v.end(),
                              [] (const auto& a, const auto& b) { return a.second < b.second; });
            return v[v.size() / 2].second;
        };
        const double outMedian = median (outPower), inMedian = median (inPower);
        std::printf ("  noise: output/input level %.1f dB\n", 10.0 * std::log10 ((outMedian + 1e-20) / (inMedian + 1e-20)));

        std::vector<std::pair<double, double>> peaks; // (Hz, 中央値に対する dB)
        for (size_t i = 1; i + 1 < outPower.size(); ++i)
            if (outPower[i].second > outPower[i - 1].second && outPower[i].second >= outPower[i + 1].second)
                if (const double db = 10.0 * std::log10 ((outPower[i].second + 1e-20) / (outMedian + 1e-20)); db > 12.0)
                    peaks.emplace_back (outPower[i].first, db);
        std::sort (peaks.begin(), peaks.end(), [] (const auto& a, const auto& b) { return a.second > b.second; });
        std::printf ("  tonal peaks in output (>12 dB above median):");
        for (size_t i = 0; i < juce::jmin ((size_t) 10, peaks.size()); ++i)
            std::printf ("  %.0f Hz (+%.0f dB)", peaks[i].first, peaks[i].second);
        std::printf (peaks.empty() ? "  none\n" : "\n");
    }
    std::fflush (nullptr);
    instance.release();
    return 0;
}
// 調査用（--analyze-f32）: 本体の --dump-taps が書いたファイル（入力 L, 入力 R, 出力 L, 出力 R を交互に）を調べ、
// 出力にあって入力にない一定の高さの音（20〜4000 Hz を 2 Hz おき）を出す
int analyzeDump (const juce::File& file, double rate)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() < 16)
        return 1;

    const auto* samples = static_cast<const float*> (data.getData());
    const size_t frames = data.getSize() / sizeof (float) / 4;
    std::vector<float> channel[4];
    for (size_t i = 0; i < frames; ++i)
        for (int ch = 0; ch < 4; ++ch)
            channel[ch].push_back (samples[i * 4 + (size_t) ch]);

    auto rms = [] (const std::vector<float>& v)
    {
        double sum = 0.0;
        for (const float s : v)
            sum += (double) s * s;
        return std::sqrt (sum / (double) juce::jmax ((size_t) 1, v.size()));
    };
    std::printf ("%s: %zu frames  in rms L %.4f R %.4f  out rms L %.4f R %.4f\n", file.getFileName().toRawUTF8(), frames,
                 rms (channel[0]), rms (channel[1]), rms (channel[2]), rms (channel[3]));

    for (int side = 0; side < 2; ++side)
    {
        const auto& in = channel[side];
        const auto& out = channel[2 + side];
        std::vector<std::pair<double, double>> peaks; // (Hz, 出力 - 入力 の dB)
        std::vector<double> outPower;
        for (double hz = 20.0; hz <= 4000.0; hz += 2.0)
            outPower.push_back (goertzelPower (out, hz, rate));
        std::vector<double> sorted (outPower);
        std::nth_element (sorted.begin(), sorted.begin() + (std::ptrdiff_t) (sorted.size() / 2), sorted.end());
        const double median = sorted[sorted.size() / 2];

        for (size_t i = 1; i + 1 < outPower.size(); ++i)
        {
            if (! (outPower[i] > outPower[i - 1] && outPower[i] >= outPower[i + 1]))
                continue;
            const double hz = 20.0 + 2.0 * (double) i;
            const double overMedian = 10.0 * std::log10 ((outPower[i] + 1e-20) / (median + 1e-20));
            const double overInput = 10.0 * std::log10 ((outPower[i] + 1e-20) / (goertzelPower (in, hz, rate) + 1e-20));
            if (overMedian > 10.0 && overInput > 10.0)
                peaks.emplace_back (hz, overInput);
        }
        std::sort (peaks.begin(), peaks.end(), [] (const auto& a, const auto& b) { return a.second > b.second; });
        std::printf ("  %s: tones in output that are not in input (>10 dB):", side == 0 ? "L" : "R");
        for (size_t i = 0; i < juce::jmin ((size_t) 12, peaks.size()); ++i)
            std::printf ("  %.0f Hz (+%.0f dB)", peaks[i].first, peaks[i].second);
        std::printf (peaks.empty() ? "  none\n" : "\n");
    }
    std::fflush (nullptr);
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    // 落ちても Windows のエラー報告の画面を出さない（本体は終了コードで失敗を知る）
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    if (argc < 3)
    {
        std::printf ("usage: PRISystemRackScanner <plugin.vst3> <output.xml>\n"
                     "       PRISystemRackScanner --probe <description.xml> <output.xml>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juce;

    // ファイルのパスは、Windows のワイド文字のコマンドラインから読む。char の argv は ANSI のコードページ（日本語の Windows では
    // CP932）で変換されているので、UTF-8 として読むと、ユーザー名が日本語の PC で一時フォルダー（%TEMP%）などのパスが壊れる。
    // 命令の名前（--probe など）と数値は ASCII なので argv のまま比べる
    juce::StringArray args;
    {
        int count = 0;
        if (wchar_t** wide = CommandLineToArgvW (GetCommandLineW(), &count))
        {
            for (int i = 0; i < count; ++i)
                args.add (juce::String (wide[i]));
            LocalFree (wide);
        }
        while (args.size() < argc)
            args.add (juce::String::fromUTF8 (argv[args.size()])); // 念のため（ワイド文字で読めなかったとき）
    }

    // VST3 プラグインの読み込み調査（PluginCheck.h）
    if (std::strcmp (argv[1], "--check") == 0 || std::strcmp (argv[1], "--check-all") == 0)
    {
        if (argc < 4)
            return 2;
        const juce::File a (args[2]), b (args[3]);
        const int code = std::strcmp (argv[1], "--check") == 0
                             ? PluginCheck::checkOne (a, b)
                             : PluginCheck::checkAll (a, b, argc >= 5 ? args[4] : juce::String());
        std::fflush (nullptr);
        TerminateProcess (GetCurrentProcess(), (UINT) code); // プラグインの後始末を待たずに終える
        return code;
    }

    if (std::strcmp (argv[1], "--analyze-f32") == 0) // --analyze-f32 <file.f32> <rate>（調査用）
    {
        const int code = argc >= 4 ? analyzeDump (juce::File (args[2]), std::atof (argv[3])) : 2;
        TerminateProcess (GetCurrentProcess(), (UINT) code);
        return code;
    }

    if (std::strcmp (argv[1], "--render-test") == 0) // --render-test <description.xml> <rate> <blockSize>（調査用）
    {
        if (argc < 5)
            return 2;
        const int code = renderTest (juce::File (args[2]), std::atof (argv[3]), std::abs (std::atoi (argv[4])),
                                     std::atoi (argv[4]) < 0, // 負の数なら長さをばらばらに
                                     argc >= 6 && std::strstr (argv[5], "host") != nullptr,   // host: 画面を開いて別のスレッドで実時間で
                                     argc >= 6 && std::strstr (argv[5], "noise") != nullptr, // noise: 左右で違うノイズを入れる
                                     argc >= 7 ? juce::File (args[6]) : juce::File()); // 設定（base64）のファイル

        TerminateProcess (GetCurrentProcess(), (UINT) code);
        return code;
    }

    if (std::strcmp (argv[1], "--probe") == 0)
    {
        if (argc < 4)
            return 2;
        const int gdiLimit = argc >= 5 ? std::atoi (argv[4]) : 2000;
        const int userLimit = argc >= 6 ? std::atoi (argv[5]) : 2000;
        const int code = safetyProbe (juce::File (args[2]), juce::File (args[3]),
                                      gdiLimit > 0 ? gdiLimit : 2000, userLimit > 0 ? userLimit : 2000,
                                      argc >= 7 && std::strcmp (argv[6], "stack") == 0); // 調査用: 呼び出し経路を <output>.stack.txt に書く
        std::fflush (nullptr);
        TerminateProcess (GetCurrentProcess(), (UINT) code);
        return code;
    }

    if (std::strcmp (argv[1], "--probe-hold") == 0)
    {
        if (argc < 4)
            return 2;
        const int holdSeconds = argc >= 5 ? std::atoi (argv[4]) : 0;
        const int code = probe (juce::File (args[2]), juce::File (args[3]), holdSeconds);
        std::fflush (nullptr);
        TerminateProcess (GetCurrentProcess(), (UINT) code); // プラグインの後始末を待たずに終える
        return code;
    }

    const juce::File pluginFile (args[1]);
    const juce::File outputFile (args[2]);

    // 先に Windows に DLL を読み込ませて、読み込めなければその理由（エラー番号）を返す（JUCE は理由を返さずに 0 個として終わるため）。
    // 読み込みの途中で落ちたら、終了コード（例外のコード）で本体に伝わる。読み込めたら JUCE がもう一度読み込む（同じものを使う）
    if (const auto binary = pri::host::ResolvePluginBinary (pluginFile.getFullPathName().toWideCharPointer()); ! binary.empty())
    {
        if (LoadLibraryW (binary.c_str()) == nullptr)
        {
            const DWORD error = GetLastError();
            juce::XmlElement root ("PLUGINS");
            root.setAttribute ("loadError", juce::String ((juce::int64) error));
            // 126（モジュールが見つからない）: 要る DLL のうち見つからないものを、原因の候補として添える
            if (error == ERROR_MOD_NOT_FOUND)
                if (const auto missing = pri::host::FindUnresolvedImports (pluginFile.getFullPathName().toWideCharPointer()); ! missing.empty())
                    root.setAttribute ("missing", juce::String (missing.c_str()));
            root.writeTo (outputFile);
            std::fflush (nullptr);
            TerminateProcess (GetCurrentProcess(), 4);
            return 4;
        }
    }

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formats.getFormats())
        format->findAllTypesForFile (types, pluginFile.getFullPathName());

    juce::XmlElement root ("PLUGINS");
    for (auto* type : types)
        root.addChildElement (type->createXml().release());

    // 書いたら、プラグインの後始末（DLL の解放）を待たずに終える。古いプラグインは解放の途中で落ちることがあり、そうなると
    // 結果は書けているのに終了コードが例外のコードになって、本体が「落ちた」と記録してしまう
    const int code = root.writeTo (outputFile) ? 0 : 1;
    std::fflush (nullptr);
    TerminateProcess (GetCurrentProcess(), (UINT) code);
    return code;
}
