#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <optional>

#include "ActivityTap.h"

// VST3 プラグインの並び（上から順に処理する）。メインのラックとアプリ用ラックの両方で使う。
//
// - OFF のプラグインと、例外で止まったプラグインには処理させない。全体バイパス中は何も通さない
// - 並びの変更、読み込み、準備、保存と復元はメッセージスレッドで行い、音声スレッドの process とは
//   chainLock で並びと作業用バッファを差し替えて受け渡す
// - 並びや ON/OFF が変わったら ChangeBroadcaster で知らせる（メッセージスレッド）
class PluginChain final : public juce::ChangeBroadcaster,
                          private juce::AsyncUpdater
{
public:
    // プラグインに渡す再生位置とテンポ。このアプリには曲の再生位置もテンポもないので、止まっている 120 BPM・4/4 として渡す。
    // 渡さないと JUCE はテンポを 0 にして渡し、テンポで割り算をするプラグインが落ちる（DodgePro は画面を作る処理と画面の処理の中で
    // 0 で割って落ちた）。音声スレッドからも呼ばれる
    class FixedPlayHead final : public juce::AudioPlayHead
    {
    public:
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (120.0);
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setTimeInSamples (0);
            info.setTimeInSeconds (0.0);
            info.setPpqPosition (0.0);
            info.setPpqPositionOfLastBarStart (0.0);
            info.setBarCount (0);
            info.setIsPlaying (false);
            info.setIsRecording (false);
            info.setIsLooping (false);
            return info;
        }
    };

    struct Plugin
    {
        int id = 0;
        std::unique_ptr<juce::AudioPluginInstance> instance; // 読み込めなかったときは nullptr（行は残す）
        juce::PluginDescription description;
        std::atomic<bool> enabled { true };

        // 処理中に例外が起きたら true（以後このプラグインには処理させない。読み込み直すと戻る）
        std::atomic<bool> faulted { false };
        bool faultLogged = false; // 音声スレッドで止めたことを app.log に書いた（メッセージスレッド）

        // 読み込めなかったときの理由と、そのときに戻すはずだった状態（保存にもこれを使う）
        juce::String loadError;
        juce::MemoryBlock savedState;

        // セーフモードで読み込まずに戻した行（instance は nullptr、状態は savedState）。ON にしたときに読み込む（setEnabled）
        bool deferredLoad = false;

        // 順に読み込む途中で、まだ読み込んでいない行（instance は nullptr、状態は savedState）。loadNextPending で読み込む
        bool pendingLoad = false;

        // プラグインの画面のウィンドウの位置と大きさ（メッセージスレッド）。なければ真ん中に出す。
        // 大きさが 0 なら位置だけ使う（置き換えたときなど）。大きさは画面が大きさを変えられるプラグインのときだけ使う
        std::optional<juce::Rectangle<int>> editorBounds;

        // 無音時の休止（setRestWhenSilent で全体を ON にしたとき）
        std::atomic<bool> alwaysProcess { false }; // このプラグインは休止させない（ノイズを足すものなど）
        std::atomic<bool> resting { false };       // 無音のため休止中（表示用。音声スレッドが書く）
        std::atomic<juce::int64> restAfterSamples { 0 }; // 休止するまでに続く必要がある無音の長さ（レイテンシーは別に足す）
        juce::int64 quietSamples = 0;              // 入力も出力も無音が続いている長さ（音声スレッドだけが使う）

        // 行のメーター（プラグイン名の右）。プラグインの前後の音（ActivityTap）から描く増減の表示。
        // その右にはエディターの画面の縮小（snapshot）を常に出す
        // 5 は以前の「エディターの画面」（今はメーターの右に常に出す）なので使わない
        enum class View { none = 0, deltaBar = 1, levelBars = 2, deltaHistory = 3, spectrum = 4,
                          outputSpectrum = 6, cpuLoad = 7, correlation = 8, goniometer = 9 };
        static bool isValidView (int value) { return (value >= 0 && value <= 4) || (value >= 6 && value <= 9); }
        View view = View::levelBars; // メッセージスレッド
        ActivityTap tap;

        // エディターの画面の静止画（エディターを開いている間と閉じる直前に撮る。メッセージスレッド）。
        // snapshotJpeg を保存し、snapshotVersion は撮るたびに増やす（表示の描き直しの合図）
        juce::Image snapshot;
        juce::MemoryBlock snapshotJpeg;
        int snapshotVersion = 0;
        void setSnapshot (const juce::Image& captured); // 縮小して JPEG にする
        void setSnapshotJpeg (const juce::MemoryBlock& jpeg);

        juce::String getDisplayName() const;
    };

    PluginChain();
    ~PluginChain() override;

    //==============================================================================
    // 並び（メッセージスレッド）
    const std::vector<std::unique_ptr<Plugin>>& getPlugins() const { return plugins; }
    Plugin* findPlugin (int id) const;
    int indexOf (int id) const;
    int countEnabled() const;

    // 1 つのラックに入れられるプラグインの数の上限。達していれば addPlugin / duplicatePlugin は足さずにエラーを返す
    static constexpr int maxPlugins = 50;
    bool isFull() const { return (int) plugins.size() >= maxPlugins; }
    static juce::String describeFull();

    // description のプラグインを index の位置に入れる（-1 または範囲外なら末尾）。
    // description は PluginScanner で調べたもの（1 つのファイルに複数のプラグインが入っていることがある）
    Plugin* addPlugin (const juce::PluginDescription& description, juce::String& errorMessage, int index = -1);
    Plugin* duplicatePlugin (int id, juce::String& errorMessage); // すぐ下に同じ設定で足す
    void removePlugin (int id);
    void movePlugin (int id, int newIndex); // newIndex は移動後の位置
    void setEnabled (int id, bool shouldBeEnabled);

    // 今の状態を控えて解放し、同じプラグインを読み込み直す（DLL も読み込み直される）。
    // 失敗しても行は残り、控えた状態は次の読み込み直しや保存に使う。プラグインの画面は先に閉じておくこと
    bool reloadPlugin (int id, juce::String& errorMessage);

    // 別のプラグインに置き換える（位置と ON/OFF は保つ）。失敗したら元のまま。プラグインの画面は先に閉じておくこと
    bool replacePlugin (int id, const juce::PluginDescription& description, juce::String& errorMessage);

    void setBypassAll (bool shouldBypass);
    bool isBypassAll() const { return bypassAll.load(); }

    // いまどこかの並びでプラグインを作っている途中か（メッセージスレッド。作っている途中でメッセージを回すプラグインがあり、
    // その間に次を作り始めると失敗するので、順に読み込むタイマーはこれが終わるのを待つ）
    static bool isCreatingInstance();

    // 無音時の休止（すべての並びに共通。既定は OFF）。ON にすると、入力が無音で、入力も出力も無音の状態が
    // レイテンシー + 余韻（テイル）+ 2 秒続いたプラグインには処理させない。音が来たらその周期から処理する
    static void setRestWhenSilent (bool shouldRest) { restWhenSilent = shouldRest; }
    static bool isRestWhenSilent() { return restWhenSilent.load(); }
    void setAlwaysProcess (int id, bool shouldAlwaysProcess); // このプラグインは休止させない
    void setView (int id, Plugin::View view);                 // 行の表示を変える

    // 新しく足したプラグインのメーター（すべての並びに共通。表示メニューで選ぶ。設定の defaultMeterView。メッセージスレッド）
    static void setDefaultView (Plugin::View view) { defaultView = view; }
    static Plugin::View getDefaultView() { return defaultView; }

    // 選んでいる再生デバイスで APO が有効か（すべての並びに共通。表示用）。有効でなければ APO から処理の依頼が
    // 来ないので、メインのラックもアプリ用ラック（メインの処理の中で鳴らす）もプラグインは動かない。
    // そのときは ON のプラグインをすべて Resting と表示する
    static void setDeviceEnabled (bool enabled) { deviceEnabled = enabled; }
    static bool isDeviceEnabled() { return deviceEnabled.load(); }

    // レイテンシー（プラグインが報告する遅延。サンプル数）
    static int getLatencySamples (const Plugin& plugin);
    int getTotalLatencySamples() const; // ON のプラグインの合計（全体バイパス中は 0）
    uint32_t getSampleRate() const { return formatRate; }
    static juce::String formatLatency (int samples, double sampleRate); // 例: "10.7 ms (512 smp)"

    //==============================================================================
    // 処理の形式（メッセージスレッド）。変わったときだけプラグインを準備し直す（その間 process は何もしない）
    void prepare (uint32_t sampleRate, uint32_t channels, uint32_t maxFrames);

    // 状態: parent に bypassAll と Plugin の子を書く / parent の Plugin の子から読む。
    // 読み込めなかったプラグインも行は残し、errors に理由を入れる。
    // loadLater なら、プラグインは読み込まずに行だけ戻し（pendingLoad）、あとで loadNextPending で 1 つずつ読み込む
    void saveState (juce::ValueTree& parent) const;
    void restoreState (const juce::ValueTree& parent, juce::StringArray& errors, bool loadLater = false);

    // まだ読み込んでいない行（pendingLoad）のうち先頭の 1 つを読み込む（メッセージスレッド）。読み込めなければ errors に理由を入れる。
    // 読み込む行がなければ false
    bool loadNextPending (juce::StringArray& errors);
    int countPendingLoads() const;

    // 一時停止（プラグインを順に読み込む間）。その間 process は何もしない（音は素通し）。全部そろったら戻して一度に切り替える
    void setSuspended (bool shouldSuspend) { suspended = shouldSuspend; }
    bool isSuspended() const { return suspended.load(); }

    //==============================================================================
    // 音声スレッド。channelData（channels 本、frames 個）をその場で処理する。
    // 形式が合わない、準備中、全体バイパス中、プラグインがないときは何もしない
    void process (float* const* channelData, uint32_t channels, uint32_t frames, uint32_t sampleRate);

private:
    void handleAsyncUpdate() override;

    // wanted のプラグインを作る。作れなければファイルを調べ直して同じプラグイン（ID か名前が同じもの）を探す
    // 作れなかったとき（理由）と、作るのに 1 秒以上かかったときは app.log に書く
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& wanted, juce::PluginDescription& description,
                                                               juce::String& errorMessage);
    std::unique_ptr<juce::AudioPluginInstance> createInstanceUnlogged (const juce::PluginDescription& wanted, juce::PluginDescription& description,
                                                                       juce::String& errorMessage);
    void logFault (const Plugin& plugin, const char* where) const; // エラーで止めたことを app.log に書く
    std::unique_ptr<Plugin> makePlugin (std::unique_ptr<juce::AudioPluginInstance> instance,
                                        const juce::PluginDescription& description, bool enabled);
    void preparePlugin (Plugin& plugin);
    void releaseInstance (Plugin& plugin); // 並びから外してから解放する
    // 解放する。例外を起こしたものは、ホスト側のロックが掛かったまま残っていて解放の途中で止まるので、解放せずに手放す
    static void disposeInstance (std::unique_ptr<juce::AudioPluginInstance> instance, bool faulted);
    void publish(); // 並びを音声スレッドに渡す

    juce::AudioPluginFormatManager formatManager;
    FixedPlayHead playHead; // すべてのプラグインに渡す（プラグインより長く生きること。プラグインはデストラクターの本体で解放する）
    std::vector<std::unique_ptr<Plugin>> plugins;
    int nextPluginId = 1;

    // プラグインを準備した形式（メッセージスレッド）
    uint32_t formatRate = 48000;
    uint32_t formatChannels = 2;
    uint32_t formatMaxFrames = 480;

    // 音声スレッドが使うもの（chainLock で守る）
    juce::SpinLock chainLock;
    std::vector<Plugin*> chain;
    juce::AudioBuffer<float> workBuffer;
    juce::AudioBuffer<float> safetyBuffer; // プラグインに渡す前の音の控え（例外のときに戻す）
    uint32_t chainRate = 0;
    uint32_t chainChannels = 0;
    uint32_t chainMaxFrames = 0;
    juce::MidiBuffer midiBuffer;

    std::atomic<bool> bypassAll { false };
    std::atomic<bool> suspended { false };
    inline static std::atomic<bool> restWhenSilent { false };
    inline static std::atomic<bool> deviceEnabled { true };
    inline static Plugin::View defaultView = Plugin::View::levelBars;
    std::atomic<bool> faultPending { false }; // 音声スレッドで例外が起きた（メッセージスレッドで知らせる）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};
