#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// VST3 ファイルの中のプラグインの一覧（PluginDescription）を調べる。
// Waves の WaveShell のように 1 つのファイルに多数のプラグインが入っていると、調べるのに 30 秒以上かかることがあるので、
// 別のプロセス（同じフォルダの PRISystemRackScanner.exe）で調べ、本体の画面は止めない。調べている途中でプラグインが落ちても本体は落ちない。
// 結果はファイルの大きさと更新日時と一緒に %APPDATA%\PRISystemRack\PluginCache.xml に保存し、ファイルが変わるまで使い回す。
// 調べられなかったファイルは、理由も一緒に保存する（プラグイン選択画面で、ファイルごとに理由を出す）。
// メッセージスレッドだけで使う
class PluginScanner
{
public:
    enum class State
    {
        idle,     // 始めていない、または取り消した
        scanning, // 調べている
        done,     // 調べ終わった（0 個のこともある）
        failed,   // 調べられなかった（理由は poll の error）
    };

    PluginScanner() = default;
    ~PluginScanner(); // 調べている途中なら止める

    // 保存してある結果を探す（ファイルの大きさと更新日時が同じときだけ）。見つかれば true（調べられなかった記録は false）
    static bool findCached (const juce::File& file, juce::Array<juce::PluginDescription>& types);

    // 前に調べられなかった（落ちた・時間がかかりすぎた）と記録してあるか（ファイルが変わっていないときだけ）。
    // プラグイン選択画面は、一覧を作り直すまでこのファイルを調べ直さない。理由を残していない記録（以前の版のもの）は false（1 回だけ調べ直す）
    static bool isKnownFailure (const juce::File& file);

    // 調べられなかった理由（PluginCache.xml の FILE の reason / a / b に残す。文は表示するときに今の言語で作る）
    //   notX64（a = CPU）、runtimeMissing（a = DLL）、runtimeTooOld（a = 必要な版、b = この PC の版）、missingDll（a = DLL）、
    //   loadError（a = Windows のエラー番号、b = 見つからない可能性のある DLL）、exitCode（a = 補助プログラムの終了コード）、crash（a = 補助プログラムの終了コード（例外のコード）。16 進）、timeout
    struct Failure
    {
        juce::String reason, a, b;
        juce::String runtimeNeed, runtimeHave; // ランタイムが古そうだった（原因の候補として理由に添える。runtimeNeed / runtimeHave で残す）
    };

    // 落ちたときの終了コード（例外のコード）の説明（よくあるものだけ。分からなければ Windows の文言か空）
    static juce::String describeExceptionCode (juce::uint32 code);

    // 調べられなかった理由（画面に出す文。記録がないとき（以前の版で記録したもの）はそう出す）
    static juce::String describeFailure (const juce::File& file);

    // 調べ始める。先にファイルの中身（実行はしない）を調べ、この PC で読み込めないと分かれば（Visual C++ ランタイムが古い、
    // 32 ビット用、要る DLL がない）補助プログラムを起動せずに失敗にする（PluginBinaryCheck）。
    // PRISystemRackScanner.exe が見つからなければ、ここでこのプロセスの中で調べ終える
    void start (const juce::File& file);

    // ときどき呼んで様子を見る。done / failed になったら types / error に結果を入れる（そのあとは idle に戻る）
    State poll (juce::Array<juce::PluginDescription>& types, juce::String& error);

    void cancel();
    double getElapsedSeconds() const;

private:
    static constexpr double timeoutSeconds = 180.0;

    void finishWith (State state, const Failure& failure = {});

    std::unique_ptr<juce::ChildProcess> process;
    juce::File scanningFile, outputFile;
    double startMs = 0.0;
    State state = State::idle;
    juce::Array<juce::PluginDescription> results;
    juce::String errorText;
    juce::String runtimeNeed, runtimeHave; // 調べているファイルのランタイムが古そうなとき（失敗したら理由に添える）
    static juce::String describeReason (const juce::XmlElement& entry); // describeFailure の、ランタイムの候補を添える前の文

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanner)
};
