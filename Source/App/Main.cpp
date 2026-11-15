#include <juce_gui_basics/juce_gui_basics.h>

#include "AlwaysOnTop.h"
#include "AppLog.h"
#include "AppTray.h"
#include "CrashLog.h"
#include "EditorGuard.h"
#include "Language.h"
#include "MainComponent.h"
#include "RackEngine.h"
#include "ScaledContent.h"
#include "Text.h"
#include "Theme.h"
#include "WindowRendering.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h> // MessageBoxW（セーフモードの確認）。JUCE のヘッダーより後に読む

class PRISystemRackApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // --quit は動いている本体に終了を頼むもの（anotherInstanceStarted）。動いていなかったときは何も起動せずに終わる
        // （インストーラーやビルドの前に、動いているか分からないまま送るため）
        if (commandLine.contains ("--quit"))
        {
            quit();
            return;
        }

        // 落ちたときに呼び出し経路とミニダンプを残す（%APPDATA%\PRISystemRack\Crashes）
        CrashLog::install();

        // プラグインが作る窓の中で例外が起きても、アプリごと落ちないようにする（構成を戻してプラグインを読み込む前に）
        EditorGuard::installForPlugins();

        juce::PropertiesFile::Options options;
        options.applicationName = "PRISystemRack";
        options.folderName = "PRISystemRack";
        options.filenameSuffix = ".settings";
        settings = std::make_unique<juce::PropertiesFile> (options);

        // 配色と文字の大きさ。言語（フォント）はこの LookAndFeel に反映するので先に設定する
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        // 既定は英語。まだ言語を選んだことがなければ、インストーラーで選んだ言語（インストーラーが
        // HKLM\SOFTWARE\PRISystemRack の InstallLanguage に en / ja を書く）で始め、設定に保存する（それからは設定に従う）
        if (! settings->containsKey ("language"))
        {
            const auto installLanguage = juce::WindowsRegistry::getValue ("HKEY_LOCAL_MACHINE\\SOFTWARE\\PRISystemRack\\InstallLanguage");
            if (installLanguage == Language::japanese || installLanguage == Language::english)
                settings->setValue ("language", installLanguage);
        }
        Language::apply (settings->getValue ("language", Language::english));

        // 表示の大きさ（既定は 100%）。選べる値の中で一番近いものにする（以前の 25% 刻みの 75% や 125% も近い値になる）。
        // 100% の基準を変えたので、以前の "uiScale" は読まずに "displayScale" を使う
        const auto savedScale = (float) settings->getDoubleValue ("displayScale", 1.0);
        Theme::uiScale = 1.0f;
        for (const auto choice : Theme::uiScaleChoices)
            if (std::abs (choice - savedScale) < std::abs (Theme::uiScale - savedScale) - 0.001f)
                Theme::uiScale = choice;

        AlwaysOnTop::enabled = settings->getBoolValue ("alwaysOnTop", false);

        // 前回、正しく終わらなかった（落ちた・強制終了された）なら、プラグインをすべて OFF にして起動するかを聞く（セーフモード）。
        // 構成を戻す前に聞く（プラグインを読み込んだり処理させたりする前に）。動いている間は印のファイルを置き、正しく終わったら消す
        const bool uncleanExit = runningMarker().existsAsFile();
        const bool safeMode = askSafeModeIfNeeded();
        AppLog::write ("started: version " + getApplicationVersion()
                       + (commandLine.isNotEmpty() ? " args=" + commandLine : juce::String())
                       + (uncleanExit ? juce::String (" | the previous run did not exit normally, safe mode: ") + (safeMode ? "yes" : "no")
                                      : juce::String()));
        runningMarker().replaceWithText (juce::String (juce::Time::getCurrentTime().toISO8601 (true)));

        // 前回の構成を戻す（画面を作り直しても読み込み直さないよう、ここで 1 回だけ）。
        // 行だけすぐ戻し、プラグインは画面を出したあとに 1 つずつ読み込む（その間は音は素通しで、画面の操作は止める。
        // 読み込めなかったプラグインは、終わったときに画面が知らせる）
        engine = std::make_unique<RackEngine>();
        engine->getOutputGuard().setEnabled (settings->getBoolValue ("outputGuard", true)); // 出力の保護（既定は ON）
        auto savedRack = settings->getXmlValue ("rack");

        // アプリ用ラックの機能（設定メニュー。既定は無効）。構成を戻す前に決める（無効ならラックは作らずに預かる）。
        // この設定がなかった頃の版から上げた人は、前回の構成にアプリ用ラックがあれば有効で始める（今の動きを変えない）
        if (! settings->containsKey ("appRacks"))
            settings->setValue ("appRacks", savedRack != nullptr && savedRack->getChildByName ("AppRack") != nullptr);
        engine->setAppRacksEnabled (settings->getBoolValue ("appRacks", false));

        if (savedRack != nullptr)
        {
            auto state = juce::ValueTree::fromXml (*savedRack);
            if (safeMode)
                turnAllPluginsOff (state);
            engine->restoreState (state);
        }

        // Windows の起動時に起動する設定なら、登録してある exe の場所を今の場所に合わせる
        AutoStart::refreshPath();

        // タイトルは「PRI System Rack - <今のプリセット>」（プリセットの部分は MainComponent が付ける）。
        // --tray（Windows の起動時の自動起動）なら、ウィンドウは出さずに通知領域のアイコンだけを出す
        const bool startHidden = commandLine.contains ("--tray");
        mainWindow = std::make_unique<MainWindow> (appDisplayName, *engine, *settings, ! startHidden);

        // 通知領域のアイコンの絵（設定メニュー。既定は静止画）
        AppTrayIcon::setStyle (settings->getIntValue ("trayIcon", (int) AppTrayIcon::Style::still) == (int) AppTrayIcon::Style::metersAndStatus
                                   ? AppTrayIcon::Style::metersAndStatus
                                   : AppTrayIcon::Style::still);
        trayIcon = std::make_unique<AppTrayIcon> (*engine);
        trayIcon->onOpen = [this] { if (mainWindow != nullptr) mainWindow->showWindow(); };
        trayIcon->addMenuItems = [this] (juce::PopupMenu& menu) { if (mainWindow != nullptr) mainWindow->addTrayMenuItems (menu); };
        trayIcon->onQuit = [this] { systemRequestedQuit(); };
        trayIcon->isWindowInFront = [this]
        {
            return mainWindow != nullptr && mainWindow->isVisible() && ! mainWindow->isMinimised() && mainWindow->isActiveWindow();
        };
        trayIcon->describeWaiting = [this] { return mainWindow != nullptr ? mainWindow->describeWaiting() : juce::String(); };
        mainWindow->onHiddenToTray = [this] { showTrayNoticeOnce(); };

        if (safeMode)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Safe mode"),
                                                    TRANS ("All plugins were turned OFF without being loaded. Each plugin is loaded when you turn it ON, "
                                                           "so turn them back ON one by one to find the plugin that caused the problem."));

        // 確認用の起動オプションは、プラグインを読み込み終わってから行う（読み込み中はまだ画面を開けず、足すこともできないため）
        mainWindow->whenPluginsLoaded ([this, commandLine]
        {
            if (mainWindow == nullptr)
                return;

            if (commandLine.contains ("--open-editors"))
                mainWindow->openAllEditors();
            if (commandLine.contains ("--open-app-racks"))
                mainWindow->openAllAppRacks();

            // --add-plugin "<.vst3 のパス>" で、メインのラックの末尾へ足す（中に複数あれば選ぶ画面が出る）
            const auto tokens = juce::StringArray::fromTokens (commandLine, true);
            // --plugin-name "<名前>" を付けると、ファイルの中（WaveShell など）のその名前のプラグインを足す
            if (const int index = tokens.indexOf ("--add-plugin"); index >= 0 && index + 1 < tokens.size())
            {
                const int nameIndex = tokens.indexOf ("--plugin-name");
                mainWindow->addPluginFile (tokens[index + 1].unquoted(),
                                           nameIndex >= 0 && nameIndex + 1 < tokens.size() ? tokens[nameIndex + 1].unquoted() : juce::String());
            }
        });
    }

    void shutdown() override
    {
        if (engine != nullptr) // 2 つ目の起動（--quit などを伝えるだけ）は initialise を通らないので書かない
            AppLog::write ("exiting");
        trayIcon.reset();
        mainWindow.reset(); // プラグインの画面を閉じて設定を保存する
        engine.reset();     // APO との接続を切ってからプラグインを解放する
        runningMarker().deleteFile(); // 正しく終わった（後片付けの途中で落ちたら印が残り、次の起動でセーフモードを聞く）
        settings.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        // 先に設定を保存する（Windows の終了・ログオフでは、shutdown まで進む前にプロセスが終わることがある）
        if (mainWindow != nullptr)
            mainWindow->saveSettings();
        if (settings != nullptr)
            settings->saveIfNeeded();

        // Windows の終了・ログオフ（WM_QUERYENDSESSION から呼ばれる）のときだけ、ここで印を消す
        // （shutdown まで進まずに終わっても、次の起動で「正しく終わらなかった」と誤って聞かないように）。
        // それ以外（通知領域の「終了」、ファイルメニュー、--quit）は shutdown の最後で消す
        if (GetSystemMetrics (SM_SHUTTINGDOWN) != 0)
        {
            AppLog::write ("Windows is shutting down or logging off");
            runningMarker().deleteFile();
        }
        quit();
    }

    // 動いている間だけ置く印のファイル（設定ファイルと同じフォルダー）。起動時に残っていれば、前回は正しく終わらなかった
    juce::File runningMarker() const
    {
        return settings != nullptr ? settings->getFile().getSiblingFile ("PRISystemRack.running") : juce::File();
    }

    // 前回が正しく終わっていなければ、プラグインをすべて OFF にして起動するかを聞く（はいなら true）。
    // 構成を戻す前（画面を作る前）に、Windows のメッセージボックスでその場で聞く
    bool askSafeModeIfNeeded() const
    {
        if (! runningMarker().existsAsFile())
            return false;

        const auto title = TRANS ("PRI System Rack did not exit normally");
        const auto message = TRANS ("PRI System Rack did not exit normally last time (it may have crashed because of a plugin).\n\n"
                                    "Start with all plugins turned OFF?\n"
                                    "Yes: start with all plugins OFF (safe mode). Turn them back ON one by one to find the cause.\n"
                                    "No: start as usual.");
        return MessageBoxW (nullptr, message.toWideCharPointer(), title.toWideCharPointer(),
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1 | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
    }

    // 構成の中のプラグインをすべて OFF にし、読み込まずに行だけ戻すようにする（メインのラックもアプリ用ラックも。並びや設定はそのまま）。
    // 前回落ちた原因が読み込み・状態の復元・準備のどれかでも、起動の途中で落ちないように。ON にしたときに読み込む（PluginChain::setEnabled）
    static void turnAllPluginsOff (juce::ValueTree tree)
    {
        if (tree.hasType ("Plugin"))
        {
            tree.setProperty ("enabled", false, nullptr);
            tree.setProperty ("deferLoad", true, nullptr);
        }
        for (auto child : tree)
            turnAllPluginsOff (child);
    }

    // 初めて × で通知領域にしまったときに 1 回だけ、動き続けていることを通知で知らせる
    void showTrayNoticeOnce()
    {
        if (trayIcon == nullptr || settings == nullptr || settings->getBoolValue ("trayNoticeShown", false))
            return;
        settings->setValue ("trayNoticeShown", true);
        trayIcon->showInfoBubble (TRANS ("PRI System Rack is still running"),
                                  TRANS ("It keeps processing the sound from the notification area. To quit, right-click the icon and choose Exit."));
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        // 調査用: 起動中に「PRISystemRack.exe --dump-taps <フォルダー>」を実行すると、メインのラックの各プラグインの
        // 直近の入力と出力（行のメーター用に貯めている約 1.3 秒）をそのフォルダーに書く（画面は前に出さない）
        const auto tokens = juce::StringArray::fromTokens (commandLine, true);

        // 「PRISystemRack.exe --quit」で、動いているアプリを終了する（× では通知領域にしまうだけなので。ビルドの前などに使う）
        if (tokens.contains ("--quit"))
        {
            systemRequestedQuit();
            return;
        }

        if (const int index = tokens.indexOf ("--dump-taps"); index >= 0 && index + 1 < tokens.size())
        {
            dumpTaps (juce::File (tokens[index + 1].unquoted()));
            return;
        }

        // 確認用: 「PRISystemRack.exe --dump-tray-icon <ファイル.png>」で、通知領域のアイコンに今出している絵を書く（画面は前に出さない）
        if (const int index = tokens.indexOf ("--dump-tray-icon"); index >= 0 && index + 1 < tokens.size())
        {
            if (trayIcon != nullptr)
            {
                const juce::File file (tokens[index + 1].unquoted());
                file.deleteFile();
                if (juce::FileOutputStream out (file); out.openedOk())
                    juce::PNGImageFormat().writeImageToStream (trayIcon->getShownImage(), out);
            }
            return;
        }

        // 確認用: 「PRISystemRack.exe --preview-overload <秒>」で、その間「処理が間に合っていない」の表示（画面の赤い帯、通知領域の説明と通知）を出す。
        // 負荷は 135% として出す（重いプラグインは本当の値）
        if (const int index = tokens.indexOf ("--preview-overload"); index >= 0 && index + 1 < tokens.size())
        {
            if (engine != nullptr)
                engine->previewOverload (tokens[index + 1].getDoubleValue());
            return;
        }

        // 確認用: 「PRISystemRack.exe --dump-tray-samples <フォルダー>」で、通知領域のアイコンのすべての状態と大きさの絵を書く
        if (const int index = tokens.indexOf ("--dump-tray-samples"); index >= 0 && index + 1 < tokens.size())
        {
            AppTrayIcon::writeSamples (juce::File (tokens[index + 1].unquoted()));
            return;
        }

        // 通知領域にしまっているときも、もう一度起動したら画面を出す
        if (mainWindow != nullptr)
            mainWindow->showWindow();
    }

    // プラグインごとに <番号>-<名前>.f32（float 32 ビット、4 チャンネルを交互に: 入力 L, 入力 R, 出力 L, 出力 R。
    // 入力と出力は同じ時刻＝レイテンシーは補正していない）と、.txt（サンプルレートとレイテンシー）を書く
    void dumpTaps (const juce::File& folder)
    {
        if (engine == nullptr)
            return;
        folder.createDirectory();

        constexpr int count = 57000;
        std::vector<float> inL (count), inR (count), outL (count), outR (count);
        int number = 0;
        for (const auto& plugin : engine->getChain().getPlugins())
        {
            ++number;
            if (! plugin->tap.read (inL.data(), inR.data(), outL.data(), outR.data(), count, 0))
                continue;

            const auto base = folder.getChildFile (juce::String (number) + "-" + juce::File::createLegalFileName (plugin->getDisplayName()));
            juce::FileOutputStream out (base.withFileExtension ("f32"));
            if (! out.openedOk())
                continue;
            out.setPosition (0);
            out.truncate();
            for (int i = 0; i < count; ++i)
                for (const float v : { inL[(size_t) i], inR[(size_t) i], outL[(size_t) i], outR[(size_t) i] })
                    out.writeFloat (v);

            base.withFileExtension ("txt").replaceWithText ("rate " + juce::String (plugin->tap.getSampleRate())
                                                          + "\nlatency " + juce::String (PluginChain::getLatencySamples (*plugin))
                                                          + "\nenabled " + juce::String ((int) plugin->enabled.load())
                                                          + "\nfresh " + juce::String ((int) plugin->tap.isFresh()) + "\n");
        }
    }

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        // visible: false なら出さずに作る（Windows の起動時の自動起動。通知領域のアイコンから showWindow で出す）
        MainWindow (const juce::String& name, RackEngine& engineToUse, juce::PropertiesFile& settingsToUse, bool visible)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons),
              engine (engineToUse),
              settings (settingsToUse)
        {
            setUsingNativeTitleBar (true);
            createContent (true);
            setResizable (true, true);
            setResizeLimits (juce::roundToInt (Theme::minWindowWidth * Theme::effectiveScale()), juce::roundToInt (Theme::minWindowHeight * Theme::effectiveScale()), 8000, 6000);
            ScaledContent::fitToScreen (*this);
            centreWithSize (getWidth(), getHeight());
            AlwaysOnTop::applyTo (*this);
            if (visible)
                showWindow();
        }

        std::function<void()> onHiddenToTray; // × で通知領域にしまったとき

        // 画面を出して前面にする（通知領域にしまっていたとき・最小化していたときも）
        void showWindow()
        {
            if (! isVisible())
                setVisible (true);
            if (isMinimised())
                setMinimised (false);
            toFront (true);
            WindowRendering::install (*this); // 普段は Direct2D（文字がくっきり）、枠のドラッグ中だけソフトウェア描画（何度呼んでもよい）
        }

        void saveSettings()
        {
            if (auto* content = getMainComponent())
                content->saveSettings();
        }

        void addTrayMenuItems (juce::PopupMenu& menu)
        {
            if (auto* content = getMainComponent())
                content->addTrayMenuItems (menu);
        }

        juce::String describeWaiting() const
        {
            if (auto* content = getMainComponent())
                return content->describeWaiting();
            return {};
        }

        void whenPluginsLoaded (std::function<void()> callback)
        {
            if (auto* content = getMainComponent())
                content->whenPluginsLoaded (std::move (callback));
        }

        void openAllEditors()
        {
            if (auto* content = getMainComponent())
                content->openAllEditors();
        }

        void openAllAppRacks()
        {
            if (auto* content = getMainComponent())
                content->openAllAppRacks();
        }

        void addPluginFile (const juce::String& path, const juce::String& name)
        {
            if (auto* content = getMainComponent())
                content->addPluginFile (path, name);
        }

        // ×: 設定の closeToTray（既定は ON）なら終了せずに隠す（音の処理は続く。通知領域のアイコンから出せる）。OFF なら終了する
        void closeButtonPressed() override
        {
            if (! settings.getBoolValue ("closeToTray", true))
            {
                JUCEApplication::getInstance()->systemRequestedQuit();
                return;
            }

            saveSettings();
            engine.stopTestTones(); // 音声テストは止める（画面がないまま鳴り続けないように。通知領域のメニューには止める項目がない）
            setVisible (false);
            if (onHiddenToTray != nullptr)
                onHiddenToTray();
        }

    private:
        MainComponent* getMainComponent() const
        {
            if (auto* scaled = dynamic_cast<ScaledContent*> (getContentComponent()))
                return dynamic_cast<MainComponent*> (scaled->getContent());
            return nullptr;
        }

        // 中身は表示の大きさ（Theme::effectiveScale()）に合わせて拡大する
        void createContent (bool resizeWindowToFit)
        {
            auto content = std::make_unique<MainComponent> (engine, settings, [this] { languageChanged(); });
            setContentOwned (new ScaledContent (std::move (content), Theme::effectiveScale()), resizeWindowToFit);
        }

        // 言語が変わったら、訳とフォントを反映して画面を作り直す（ウィンドウの大きさは保つ）
        void languageChanged()
        {
            Language::apply (settings.getValue ("language", Language::english));
            const auto bounds = getContentComponent() != nullptr ? getContentComponent()->getBounds() : juce::Rectangle<int>();
            clearContentComponent(); // 古い画面は設定を保存し、プラグインの画面を閉じる
            createContent (false);
            if (! bounds.isEmpty())
                getContentComponent()->setBounds (bounds);
        }

        RackEngine& engine;
        juce::PropertiesFile& settings;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    AppLookAndFeel lookAndFeel; // ウィンドウより長く生きること
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<RackEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<AppTrayIcon> trayIcon; // 通知領域のアイコン（動いている間は常に出す）
};

START_JUCE_APPLICATION (PRISystemRackApplication)
