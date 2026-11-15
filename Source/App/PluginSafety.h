#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// プラグイン安全チェック（Plugin safety check）。設定メニューで ON/OFF（設定の pluginSafetyCheck。既定は ON）。
// 足す・置き換えるときに、本体に読み込む前に、補助プログラム（PRISystemRackScanner.exe --probe）の中で同じ手順で 1 つ作って試し、
// 次のどれかなら本体に読み込まない:
//   - ファイルの中身: 64 ビット用でない、Visual C++ ランタイムがない・古い
//   - Windows が DLL を読み込めない（エラー番号）、作れない（JUCE のエラー）
//   - 作る・準備する・無音を処理する・設定を保存して戻すのどこかで落ちた、時間内（timeoutSeconds）に終わらない
//   - 描画資源（GDI / USER オブジェクト）を上限（gdiThreshold / userThreshold）以上使う。使い切ると解放しても戻らず、
//     窓やカーソルを作れなくなって画面が描けない・落ちる（WaveShell 10.0 の Vocal Rider、L3 など）ので、本体の中では試せない
// 読み込まないと分かったもののうち、何度試しても同じになるもの（64 ビット用でない、描画資源を使い切る）は
// %APPDATA%\PRISystemRack\PluginSafety.xml にも残し（プラグインのファイルが変わったら捨てて試し直す）、
// 起動し直してもプリセットの読み込みなどで作らず、プラグインの選択画面の一覧に印を出す（選ぶか［プラグインを更新］でもう一度チェックできる）。
// それ以外（時間切れ、落ちた、ランタイムがないなど）は PC の状態で変わることがあるので、起動している間だけ覚える。
// 問題なかったものは、アプリを起動している間だけ覚えておく（2 回目からは試さずに読み込む）。メッセージスレッドだけで使う
namespace PluginSafety
{
constexpr int gdiThreshold = 2000;  // 普通のプラグインは 1 つで数十〜数百
constexpr int userThreshold = 2000; // 同じ（Vocal Rider は約 5,700）
constexpr double timeoutSeconds = 120.0;

void setEnabled (bool enabled);
bool isEnabled();

// 試した結果
struct Verdict
{
    bool blocked = false;
    // ok / binary / loadError / createFailed / crashed / timeout / noResult / tooManyGdi / tooManyUser
    juce::String reason;
    juce::String stage;      // 失敗した段階（binary / load / create / prepare / process / state）
    juce::String a, b, c;    // 理由ごとの詳しい値（binary: 種類と値、loadError: 番号と DLL、createFailed: エラー、crashed: 例外のコード（16 進））
    int gdi = 0, user = 0;   // 増えた GDI / USER オブジェクトの数（分かっていれば）
    // ファイルの中身から、この PC の Visual C++ ランタイムが古そうだと分かっていた（試しは続け、失敗したら原因の候補として添える）
    juce::String runtimeNeed, runtimeHave;
    // 試したプラグインのファイルと、そのときの目印（大きさと更新日時）。読み込めないと分かったものを PluginSafety.xml に残し、
    // 起動し直したときに、ファイルが変わっていなければ使う
    juce::String file, fileStamp;
};

// 起動している間に試して、読み込まないと分かったか（チェックが OFF なら false）
bool isBlocked (const juce::PluginDescription& description);

// 起動している間に試した結果（試していなければ nullptr）
const Verdict* findVerdict (const juce::PluginDescription& description);

// 試した結果を忘れる（もう一度試すとき）
void forget (const juce::PluginDescription& description);

// 読み込まなかった理由（画面に出す文）と、一覧の印に出す短い理由
juce::String describe (const Verdict& verdict, const juce::String& pluginName);
juce::String describeShort (const Verdict& verdict);

// 段階の表示名（binary / load / create / prepare / process / state）
juce::String stageName (const juce::String& stage);

// 補助プログラムでの試し読み込み 1 回分。start のあと、タイマーなどから poll を呼んで様子を見る（待たない）
class Probe
{
public:
    struct Progress
    {
        juce::String stage;   // 今の段階（まだなら空）
        int gdi = -1, user = -1;
        double seconds = 0.0; // 始めてからの時間
    };

    Probe() = default;
    ~Probe(); // 途中なら止める

    // 補助プログラムを起動する。起動できなければ false（試せない）
    bool start (const juce::PluginDescription& description);

    // 終わったら true を返し、verdict に結果を入れて、起動している間の結果として覚える（取り消したときは覚えない）
    bool poll (Progress& progress, Verdict& verdict);

    void cancel();

private:
    juce::PluginDescription description;
    std::unique_ptr<juce::ChildProcess> process;
    juce::File descriptionFile, outputFile;
    double startMs = 0.0;
    Progress last;

    void cleanUp();
    JUCE_DECLARE_NON_COPYABLE (Probe)
};
} // namespace PluginSafety
