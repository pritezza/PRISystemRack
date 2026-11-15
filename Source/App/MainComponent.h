#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <set>

#include "AppRackWindow.h"
#include "ChainController.h"
#include "ChainView.h"
#include "HostCore/DeviceWatcher.h"
#include "HostCore/Endpoints.h"
#include "ChainPresets.h"
#include "PluginScanner.h"
#include "PresetManager.h"
#include "NumberBox.h"
#include "RackEngine.h"
#include "ScaledContent.h"
#include "EnableSwitch.h"
#include "TestToneToggles.h"

// メイン画面。構成は RackEngine が持ち、この画面は言語を切り替えると作り直される
// （アプリ用ラックのウィンドウも作り直されるが、ラックそのものは動き続ける）。
class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener,
                            private juce::MenuBarModel
{
public:
    // onLanguageChanged: 言語が選ばれたら呼ばれる（設定は保存済み）。呼び出し元が画面を作り直す
    MainComponent (RackEngine& engineToUse, juce::PropertiesFile& settingsToUse, std::function<void()> onLanguageChanged);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void saveSettings();

    // 通知領域のアイコンの右クリックのメニューに、バイパスとプリセットの項目を足す
    void addTrayMenuItems (juce::PopupMenu& menu);

    // 有効にしてあるのに処理できず待っている理由（選んだ再生デバイスが接続されていない、APO とつながるのを待っている）。
    // 待っていなければ空。ステータスバーで注意の色にしている場面と同じ。通知領域のアイコンを注意の色（黄）にするのに使う
    juce::String describeWaiting() const;

    // すべてのプラグインの画面を開く（起動オプション --open-editors 用）
    void openAllEditors() { controller.openAllEditors(); }

    // プラグインを読み込み終わってから callback を呼ぶ（読み込み中でなければすぐ。起動時の確認用オプションに使う）
    void whenPluginsLoaded (std::function<void()> callback);

    // メインのラックの末尾にプラグインのファイルを足す（起動オプション --add-plugin 用）。
    // name があれば、ファイルの中（WaveShell など）のその名前のプラグインを、調べた結果（PluginCache.xml）から選んで足す
    void addPluginFile (const juce::String& path, const juce::String& name = {})
    {
        juce::Array<juce::PluginDescription> types;
        if (name.isNotEmpty() && PluginScanner::findCached (juce::File (path), types))
            for (const auto& type : types)
                if (type.name == name)
                {
                    controller.addChosen (type);
                    return;
                }
        controller.addPluginFiles ({ path }, -1);
    }

    // すべてのアプリ用ラックのウィンドウを開く（起動オプション --open-app-racks 用）
    void openAllAppRacks()
    {
        for (const auto& rack : engine.getAppRacks())
            openAppRack (rack->getId());
    }

private:
    class RackChip;

    // 一覧を開く直前に onBeforePopup を呼ぶ選択欄（再生デバイスの一覧を開くたびに読み直すため）
    class RefreshingComboBox final : public juce::ComboBox
    {
    public:
        std::function<void()> onBeforePopup;

        void showPopup() override
        {
            if (onBeforePopup != nullptr)
                onBeforePopup();
            ComboBox::showPopup();
        }
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void refreshEndpoints();
    void endpointChosen();
    void updateStatus();
    void updateEnableState();
    void updateBypassButton();
    void enableClicked();
    void runSetup (const juce::String& arguments, const juce::String& successNotice);
    const pri::host::RenderEndpoint* selectedEndpoint() const;
    juce::String previousApoName (const pri::host::RenderEndpoint& endpoint) const;
    void chooseLanguage (const juce::String& code);
    void chooseUiScale (float scale);

    // 上端のメニューバー（ファイル / 表示 / ヘルプ）。画面の中に置くので、表示の大きさに合わせて拡大される
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected (int, int) override {} // 項目はそれぞれの処理（PopupMenu の action）で動く
    juce::PopupMenu createFileMenu();
    juce::PopupMenu createViewMenu();
    juce::PopupMenu createSettingsMenu();
    juce::PopupMenu createHelpMenu();
    void openLogFolder();
    void showAbout();

    void rebuildRackChips();
    void setAppRacksEnabled (bool shouldBeEnabled); // 設定メニューの「アプリ用ラックを使う」
    void addAppRack();
    void openAppRack (int rackId);
    void deleteAppRack (int rackId);
    void confirmDeleteAppRack (int rackId);

    RackEngine& engine;
    juce::PropertiesFile& settings;
    std::function<void()> languageChanged;
    ChainController controller;

    juce::MenuBarComponent menuBar;
    juce::Label endpointLabel, racksLabel;
    RefreshingComboBox endpointBox;
    // 選んでいる再生デバイスで使うかの切り替え [ 有効 | 無効 ]（今の状態の表示を兼ねる。APO を入れる / 外す。
    // 管理者の許可と音声サービスの再起動が要る）
    EnableSwitch enableSwitch;
    juce::String setupNotice;          // 切り替えの結果（ステータスバーにしばらく出す）
    juce::uint32 setupNoticeUntilMs = 0;
    juce::Colour setupNoticeColour = juce::Colours::lightgrey;
    juce::String loggedStatusKind; // 最後に app.log に書いたステータスの種類（updateStatus）
    bool loggedOverload = false;   // 処理が間に合っていない状態を app.log に書いた（終わったら書いて戻す）
    // 左クリックでだけ押せるボタン（右クリックでは何もしない）
    class LeftClickButton final : public juce::TextButton
    {
    public:
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isLeftButtonDown())
                TextButton::mouseDown (e);
        }
        void mouseDrag (const juce::MouseEvent& e) override // 右ボタンのドラッグで押された状態にならないように
        {
            if (e.mods.isLeftButtonDown())
                TextButton::mouseDrag (e);
        }
    };

    juce::TextButton bypassButton;
    LeftClickButton addRackButton; // ＋ ラックを追加
    // 上の欄の 2 行目「バッファ」: 量（上下ドラッグ・クリックで入力・ホイール）、自動、無音時に休止
    juce::Label bufferLabel;
    NumberBox bufferValue;               // 処理バッファ 0〜200 ms（設定の processingBufferMs）
    juce::ToggleButton bufferAutoToggle; // 自動で増減（設定の processingBufferAuto）
    juce::ToggleButton restToggle;       // 無音時にプラグインを休止（すべてのラックに共通。設定の restWhenSilent）
    TestToneToggles testToggles { engine }; // 音声テスト（入力 / 出力）。バッファの行の右
    // プリセット: メインのラックとアプリ用ラックの構成まるごと（ChainPresets.xml）。選ぶ・保存するなどはファイルメニュー
    ChainPresets presets;
    juce::String currentPresetId; // 最後に読み込んだか保存したプリセット（設定の currentPreset）
    void refreshPresets();        // 今のプリセットがまだあるかを見直して保存する（管理の画面で消されたときなど）
    void loadPreset (const juce::String& id);
    void confirmNewRack(); // ファイルメニューの「新規作成」（確認してから newRack）
    void newRack();        // まっさらな状態にする（プラグインとアプリ用ラックをすべて外す）
    void saveNewPreset();
    void overwritePreset();
    void updatePresetModified(); // 今の構成が基準と違うかを見直す（ファイルメニューの「上書き保存」を押せるか）とタイトルバーの更新
    void updateWindowTitle();    // タイトルバーに今のプリセットと変更の有無を出す
    void takePresetBaseline();   // 今の構成を「変更なし」の基準にする（読み込み・保存の直後）
    bool presetModified = false;

    // プラグインを 1 つずつ読み込む間（RackEngine::restoreState のあと）。その間は画面の操作をすべて止め、
    // 読み込み終わったら「変更なし」の基準を取り、読み込めなかったプラグインを知らせる
    void startedLoading();     // 構成を読み込み始めた（プリセット・新規作成）
    void pluginsLoaded();      // 読み込みが終わった（RackEngine::onPluginsLoaded）
    void updateLoadingLock();  // 読み込み中なら画面を操作できなくする
    bool baselineAfterLoad = false; // 読み込み終わったら takePresetBaseline する
    std::vector<std::function<void()>> afterLoading; // 読み込み終わったら呼ぶもの（whenPluginsLoaded）

    // 「変更なし」の基準: 並び・ON/OFF・ゲイン・アプリ用ラックなど（プラグインの設定データは除く）。
    // プラグインの設定データは書き出すたびに一部が変わるものがあるので（MReverbMB で確認）比べず、
    // プラグインからの「パラメーターが変わった」「プリセットが変わった」の知らせで変更を見る（PluginEditWatcher）
    std::unique_ptr<juce::XmlElement> presetBaseline;

    class PluginEditWatcher final : private juce::AudioProcessorListener
    {
    public:
        ~PluginEditWatcher() override;
        void sync (RackEngine& engine);      // 今あるプラグインすべてに知らせを頼む（メッセージスレッド）
        void reset (int ignoreMilliseconds); // 変更なしに戻し、しばらく知らせを無視する（読み込みの直後に届く分）
        bool isDirty() const { return dirty.load(); }
        void detachAll (RackEngine& engine); // 画面を消すとき、まだあるプラグインから外す

    private:
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override { mark(); }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (details.programChanged || details.nonParameterStateChanged)
                mark();
        }
        void mark()
        {
            if ((juce::int32) (juce::Time::getMillisecondCounter() - ignoreUntilMs.load()) >= 0)
                dirty = true;
        }

        std::atomic<bool> dirty { false };
        std::atomic<juce::uint32> ignoreUntilMs { 0 };
        std::set<std::pair<int, juce::AudioPluginInstance*>> watched; // (プラグインの ID, インスタンス)
    };
    PluginEditWatcher editWatcher;
    void duplicatePreset();
    void renamePreset();
    void deletePreset();
    void managePresets();
    void askPresetName (const juce::String& title, const juce::String& initialName, std::function<void (const juce::String&)> onAccepted);

    int pendingBufferMs = -1;            // 手動で変えた量。変更が 0.3 秒止まってから反映する（つなぎ直しや途切れを減らすため）
    juce::uint32 bufferChangedAtMs = 0;
    void applyBufferSettings();
    bool setupRunning = false;
    juce::Label statusLabel;

    // 処理が間に合っていないときに、ラックとステータスバーの間に出す赤い帯（RackEngine::checkOverload）。
    // 何が起きているか（1 行目）と、負荷・重いプラグイン・対処（2 行目以降）を書く。幅に合わせて折り返し、高さも合わせる
    class OverloadBanner final : public juce::Component
    {
    public:
        bool setText (const juce::String& newTitle, const juce::String& newDetail); // 変わったら true
        int getHeightForWidth (int width) const;
        void paint (juce::Graphics& g) override;

    private:
        juce::TextLayout layoutFor (float width) const;
        juce::String title, detail;
    };
    OverloadBanner overloadBanner;
    void updateOverloadBanner();

    // プラグインを読み込んでいる間（起動時・プリセットを開いたとき）、画面全体を暗くして、中央に進み具合のカードを出す。
    // 1 つ読み込んでいる間は画面が止まる（VST3 は画面のスレッドで作る決まり）ので、表示は 1 つ終わるたびに変わる
    class LoadingOverlay final : public juce::Component
    {
    public:
        bool setProgress (int done, int total, const juce::String& next); // 変わったら true
        void paint (juce::Graphics& g) override;

    private:
        int done = 0, total = 0;
        juce::String next;
    };
    LoadingOverlay loadingOverlay;
    void updateLoadingOverlay();
    juce::Viewport viewport;
    ChainView chainView;

    std::vector<std::unique_ptr<RackChip>> rackChips;
    std::map<int, std::unique_ptr<AppRackWindow>> rackWindows;
    std::vector<pri::host::RenderEndpoint> endpoints;

    // 再生デバイスの変化の通知。届いたら、通知が 300 ms 途切れてから一覧を読み直す（まとめて来るので）
    pri::host::DeviceWatcher deviceWatcher;
    uint32_t seenDeviceChanges = 0;
    juce::uint32 lastDeviceChangeMs = 0;
    bool deviceListStale = false;
    bool selectedEndpointMissing = false; // 選んでいたデバイスが一覧にない（戻ってきたら接続し直す）

    bool settingsDirty = false;
    int timerTicks = 0;

    // ツールチップの表示役。この画面の子にするので、ScaledContent の倍率で文字も拡大される
    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
