#include "MainComponent.h"

#include "AlwaysOnTop.h"
#include "AppLog.h"
#include "CrashLog.h"
#include "Language.h"
#include "PluginActivityView.h"
#include "PluginBrowser.h"
#include "PluginSafety.h"
#include "SectionMeter.h"
#include "AppTray.h"
#include "Text.h"
#include "Theme.h"

#include "HostCore/Setup.h"

namespace
{
constexpr int timerHz = 20;
// 上の欄: 3 行（再生デバイス / バッファ / アプリ用ラック）と行の間、上下左右の余白
constexpr int headerPadX = 12;
constexpr int headerPadY = 5;
constexpr int maxEndpointBoxWidth = 220; // 再生デバイスの選択欄（一覧はデバイス名の長さまで広がる）
constexpr int rowGap = 2;
constexpr int headerRowHeight = 20; // 上の欄の 1 行の高さ（ラックの札の高さと同じ）
constexpr int slimBoxHeight = 18;   // 上の欄の選択欄・数値の欄・ボタンの高さ（行の中で上下中央に置く。文字の大きさは変えない）
constexpr int racksLineGap = 5; // 3 行目のアプリ用ラックの札を折り返したときの行の間（上の欄の高さは札の行の数で変わる）

// 上端のメニューバー（ファイル / 表示 / ヘルプ）
constexpr int menuBarHeight = 22;

// 下端のステータスバー（メッセージ）
constexpr int statusBarHeight = 25;

const juce::Colour okColour = Theme::ok;
const juce::Colour warningColour = Theme::warning;
const juce::Colour idleColour = Theme::dimText;

const juce::String setupLogPath ("%ProgramData%\\PRISystemRack\\Logs\\setup.log");
const juce::String devInstallScript ("Tools\\Scripts\\dev-install.ps1");

juce::String describeConnectError (uint32_t error)
{
    switch (error)
    {
        case 2:    return TRANS ("the APO is not installed on this device, or nothing is playing yet");
        case 5:    return TRANS ("no permission to open the shared memory");
        case 170:  return TRANS ("another PRI System Rack is processing this device");
        case 1306: return TRANS ("the APO and the application versions do not match");
        default:   return TRANS ("error %code%").replace ("%code%", juce::String (error));
    }
}

void styleButton (juce::TextButton& button)
{
    Theme::styleButton (button);
}

// 入力・出力の欄のメーターの種類とピークホールドを保存する。表示メニューからも、メーターのホイール・右クリックからも、ここを通す
void saveSectionMeterSettings (juce::PropertiesFile& settings)
{
    settings.setValue ("sectionMeter", ChainView::getSectionMeterKind());
    settings.setValue ("peakHold", ChainView::isPeakHoldEnabled());
    settings.saveIfNeeded();
}
} // namespace

//==============================================================================
// 上の欄に並べる、アプリ用ラック 1 つ分の札（状態の点と名前）
// 左の点（状態の色。バイパス中は迂回の矢印）のクリックでラックのバイパスを切り替え、それ以外の場所のダブルクリックでラックの画面を開く。
// 右クリックで「削除」のメニュー
class MainComponent::RackChip final : public juce::Component,
                                      public juce::TooltipClient
{
public:
    explicit RackChip (AppRack& rackToShow) : rack (rackToShow)
    {
        setTitle (rack.getName());
    }

    std::function<void()> onOpen;   // ダブルクリック（ラックの画面を開く）
    std::function<void()> onDelete; // 右クリックのメニューの「削除」（確認は呼び出し先で行う）

    int getRackId() const { return rack.getId(); }

    // 札の高さと名前の文字（上の欄の 1 行より少し小さい）
    static constexpr int chipHeight = 20;
    static constexpr float chipTextSize = Theme::smallSize;

    int preferredWidth() const
    {
        const juce::FontOptions bold (chipTextSize, juce::Font::bold);
        return textX + juce::GlyphArrangement::getStringWidthInt (bold, rack.getName()) + 12;
    }

    void paint (juce::Graphics& g) override
    {
        const bool bypassed = rack.getChain().isBypassAll();
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Theme::mergeFill.brighter (hoverName ? 0.12f : 0.0f));
        g.fillRoundedRectangle (area, 7.0f);
        g.setColour (bypassed ? Theme::bypassOn : Theme::mergeBorder);
        g.drawRoundedRectangle (area, 7.0f, 1.5f);

        // 点の上にいるときは、押せることが分かるように点の周りを明るくする
        const float cy = (float) getHeight() * 0.5f;
        if (hoverDot)
        {
            g.setColour (juce::Colours::white.withAlpha (pressedDot ? 0.22f : 0.12f));
            g.fillEllipse (dotCentreX - 9.0f, cy - 9.0f, 18.0f, 18.0f);
        }

        if (bypassed)
        {
            drawBypassIcon (g, dotCentreX, cy, Theme::bypassOn);
        }
        else
        {
            const auto& status = rack.getStatus();
            const auto dot = status.doubled || status.captureError.isNotEmpty() ? warningColour
                           : ! status.runningTargets.isEmpty()                   ? okColour
                                                                                  : idleColour;
            g.setColour (dot);
            g.fillEllipse (dotCentreX - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        }

        // ラックの名前
        g.setColour (bypassed ? Theme::subText : Theme::text);
        g.setFont (juce::FontOptions (chipTextSize, juce::Font::bold));
        g.drawText (rack.getName(), textX, 0, getWidth() - textX - 10, getHeight(), juce::Justification::centredLeft, true);
    }

    juce::String getTooltip() override
    {
        if (isOverDot (getMouseXYRelative()))
            return rack.getChain().isBypassAll()
                       ? TRANS ("Bypassed: the audio of this rack passes through without its plugins. Click to turn bypass off.")
                       : TRANS ("Click to bypass the plugins of this rack (the audio still passes through)");
        return TRANS ("Double-click to open the window of this app rack");
    }

    void mouseMove (const juce::MouseEvent& e) override { updateHover (e.getPosition()); }
    void mouseEnter (const juce::MouseEvent& e) override { updateHover (e.getPosition()); }
    void mouseExit (const juce::MouseEvent&) override { updateHover ({ -1, -1 }); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // 右クリック（どこでも）: 削除のメニュー
        if (e.mods.isPopupMenu())
        {
            pressedDot = false;
            juce::PopupMenu menu;
            menu.addItem (TRANS ("Delete this rack"), [safeThis = juce::Component::SafePointer<RackChip> (this)]
            {
                if (safeThis != nullptr && safeThis->onDelete != nullptr)
                    safeThis->onDelete();
            });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
            return;
        }

        pressedDot = e.mods.isLeftButtonDown() && isOverDot (e.getPosition());
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool clicked = pressedDot && isOverDot (e.getPosition()) && ! e.mouseWasDraggedSinceMouseDown();
        pressedDot = false;
        if (clicked)
            rack.getChain().setBypassAll (! rack.getChain().isBypassAll());
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown() && ! isOverDot (e.getPosition()) && onOpen != nullptr)
            onOpen();
    }

private:
    static constexpr float dotCentreX = 14.0f;
    static constexpr int dotHitRight = 25; // 点を押せる範囲の右端（点より少し広め）
    static constexpr int textX = 26;

    bool isOverDot (juce::Point<int> p) const { return p.x >= 0 && p.x < dotHitRight && getLocalBounds().contains (p); }

    void updateHover (juce::Point<int> p)
    {
        const bool dot = isOverDot (p);
        const bool name = ! dot && getLocalBounds().contains (p);
        setMouseCursor (dot ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        if (dot != hoverDot || name != hoverName)
        {
            hoverDot = dot;
            hoverName = name;
            repaint();
        }
    }

    // 迂回の矢印: 左から来た線が、真ん中の箱（プラグイン）を飛び越えて右へ抜ける
    static void drawBypassIcon (juce::Graphics& g, float cx, float cy, juce::Colour colour)
    {
        const float left = cx - 8.0f, right = cx + 8.0f;
        const float base = cy + 4.0f, top = cy - 6.5f;

        // 飛び越される箱（薄く）
        g.setColour (colour.withAlpha (0.45f));
        g.fillRoundedRectangle (cx - 2.5f, base - 4.0f, 5.0f, 5.0f, 1.0f);

        juce::Path arc;
        arc.startNewSubPath (left, base);
        arc.lineTo (left + 2.5f, base);
        arc.cubicTo (left + 2.5f, top, right - 2.5f, top, right - 2.5f, base - 2.5f);
        g.setColour (colour);
        g.strokePath (arc, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // 矢印の先（下向き）
        juce::Path head;
        head.addTriangle (right - 6.0f, base - 3.5f, right + 1.0f, base - 3.5f, right - 2.5f, base + 1.0f);
        g.fillPath (head);
    }

    AppRack& rack;
    bool hoverDot = false, hoverName = false, pressedDot = false;
};

//==============================================================================
MainComponent::MainComponent (RackEngine& engineToUse, juce::PropertiesFile& settingsToUse, std::function<void()> onLanguageChanged)
    : engine (engineToUse),
      settings (settingsToUse),
      languageChanged (std::move (onLanguageChanged)),
      controller (engineToUse.getChain(), {}, [this] { viewport.setViewPosition (0, chainView.getHeight()); }),
      chainView (engineToUse, controller.callbacks())
{
    menuBar.setModel (this);
    addAndMakeVisible (menuBar);

    endpointLabel.setText (TRANS ("Playback device"), juce::dontSendNotification);
    racksLabel.setText (TRANS ("App racks"), juce::dontSendNotification);
    for (auto* label : { &endpointLabel, &racksLabel })
    {
        label->setColour (juce::Label::textColourId, Theme::subText);
        label->setFont (juce::FontOptions (Theme::bodySize));
        label->setBorderSize ({ 1, 0, 1, 0 }); // 左右の内側の余白なし（見出しの列を文字の幅に詰めるため）
        addAndMakeVisible (label);
    }

    endpointBox.setTitle (TRANS ("Playback device"));
    endpointBox.onChange = [this] { endpointChosen(); };
    endpointBox.onBeforePopup = [this] { refreshEndpoints(); }; // 一覧を開くたびに読み直す（「一覧を更新」ボタンの代わり）
    addAndMakeVisible (endpointBox);
    seenDeviceChanges = deviceWatcher.GetChangeCount();

    // 切り替え [ 有効 | 無効 ]。今の状態の側が色付きで、反対側（盾付き）を押すと UAC の確認が出て、許可すると APO を入れ替える
    // （UAC の確認が出ない設定のときは、代わりにアプリが確認する）。終わったらデバイスの一覧を読み直して、実際の状態を表示に映す
    enableSwitch.setTitle (TRANS ("Use on this device"));
    enableSwitch.onSwitchRequested = [this] { enableClicked(); };
    addAndMakeVisible (enableSwitch);

    bypassButton.setButtonText (TRANS ("Bypass"));
    bypassButton.setClickingTogglesState (true);
    bypassButton.setTooltip (TRANS ("Passes the sound through without the plugins (gain is still applied). Switches instantly without stopping the sound."));
    bypassButton.onClick = [this] { engine.getChain().setBypassAll (bypassButton.getToggleState()); };
    addAndMakeVisible (bypassButton);

    addRackButton.setButtonText (TRANS ("+ Add rack"));
    addRackButton.setColour (juce::TextButton::buttonColourId, Theme::windowBackground);
    addRackButton.setColour (juce::TextButton::textColourOffId, Theme::mergeText);
    addRackButton.setColour (juce::ComboBox::outlineColourId, Theme::mergeBorder);
    addRackButton.setTooltip (TRANS ("Capture the audio of specific apps, process it with its own plugins and merge it into the main rack"));
    addRackButton.onClick = [this] { addAppRack(); };
    addRackButton.getProperties().set ("textSize", RackChip::chipTextSize); // 文字はラックの札にそろえる
    addAndMakeVisible (addRackButton);

    // 無音時の休止（既定は OFF）
    PluginChain::setRestWhenSilent (settings.getBoolValue ("restWhenSilent", false));
    restToggle.setButtonText (TRANS ("Rest when silent"));
    restToggle.setTooltip (TRANS ("Plugins do not process while the audio is silent (after their tail ends), to save CPU. "
                                  "Use ... > Always process for plugins that make sound from silence, such as noise generators."));
    restToggle.setToggleState (PluginChain::isRestWhenSilent(), juce::dontSendNotification);
    Theme::makeCompactToggle (restToggle);
    restToggle.onClick = [this]
    {
        PluginChain::setRestWhenSilent (restToggle.getToggleState());
        settings.setValue ("restWhenSilent", restToggle.getToggleState());
        settings.saveIfNeeded();
    };
    addAndMakeVisible (restToggle);
    addAndMakeVisible (testToggles);

    // 上の欄の 2 行目「バッファ」: 自動、量（スピンボックス）、無音時に休止
    bufferLabel.setText (TRANS ("Buffer"), juce::dontSendNotification);
    bufferLabel.setColour (juce::Label::textColourId, Theme::subText);
    bufferLabel.setFont (juce::FontOptions (Theme::bodySize));
    bufferLabel.setBorderSize ({ 1, 0, 1, 0 });
    addAndMakeVisible (bufferLabel);

    // 処理バッファ（既定はオフ）。APO とつなぐ前に決めておく
    {
        const auto tooltip = TRANS ("Processing buffer: plugins are processed on a separate thread ahead of time, so heavy plugins that sometimes take longer "
                                    "than one period (such as Waves Clarity Vx) do not cause dropouts. Adds this time to the latency. 0 ms = off.");
        bufferValue.setRange (0, ProcessingBuffer::maxMilliseconds);
        bufferValue.setSuffix (" ms");
        bufferValue.setValue (settings.getIntValue ("processingBufferMs", 0));
        bufferValue.setTooltip (tooltip + " " + TRANS ("Drag up or down to change, click to type a value."));
        bufferValue.setTitle (TRANS ("Buffer"));
        bufferValue.onValueChange = [this] (int ms)
        {
            pendingBufferMs = ms;
            bufferChangedAtMs = juce::Time::getMillisecondCounter();
        };
        addAndMakeVisible (bufferValue);
        bufferLabel.setTooltip (tooltip);

        bufferAutoToggle.setButtonText (TRANS ("Auto"));
        bufferAutoToggle.setTooltip (TRANS ("Increases the buffer when a dropout is detected, and reduces it slowly (during silence) when there is plenty of headroom."));
        bufferAutoToggle.setToggleState (settings.getBoolValue ("processingBufferAuto", false), juce::dontSendNotification);
        Theme::makeCompactToggle (bufferAutoToggle);
        bufferAutoToggle.onClick = [this]
        {
            // 自動をやめたときは、自動で決まっていた量を手動の量として引き継ぐ
            if (! bufferAutoToggle.getToggleState() && engine.isProcessingBufferOn())
                bufferValue.setValue (juce::roundToInt (engine.getProcessingBufferCurrentMs()));
            pendingBufferMs = -1;
            applyBufferSettings();
        };
        addAndMakeVisible (bufferAutoToggle);

        bufferValue.setEnabled (! bufferAutoToggle.getToggleState());
        engine.setProcessingBuffer (bufferValue.getValue(), bufferAutoToggle.getToggleState());
    }

    // プリセット（メインのラックとアプリ用ラックの構成まるごと。ファイルメニューで選ぶ・保存する）
    currentPresetId = settings.getValue ("currentPreset");
    if (! presets.contains (currentPresetId))
        currentPresetId = {};
    // 前回の終了時に変更がなかったなら、今の構成（起動時に戻したもの）を基準にする。変更があったなら基準なし（= 変更あり）。
    // プラグインを読み込んでいる途中なら、読み込み終わってから基準にする（pluginsLoaded）
    if (currentPresetId.isNotEmpty() && ! settings.getBoolValue ("presetModified", false))
    {
        if (engine.isLoadingPlugins())
            baselineAfterLoad = true;
        else
            takePresetBaseline();
    }
    else if (currentPresetId.isNotEmpty())
    {
        presetModified = true;
    }
    refreshPresets();

    // プラグインを 1 つずつ読み込んでいる間（起動時・プリセットの読み込み）は、画面の操作をすべて止める
    engine.onPluginsLoaded = [this] { pluginsLoaded(); };
    updateLoadingLock();

    // プラグイン安全チェック（設定メニュー。既定は ON）
    PluginSafety::setEnabled (settings.getBoolValue ("pluginSafetyCheck", true));

    // 入力・出力の欄のメーターのピークホールド（表示メニュー。既定は ON）
    ChainView::setPeakHoldEnabled (settings.getBoolValue ("peakHold", true));

    // 入力・出力の欄のメーターの種類（メーターの上のホイールか右クリックで変える。入力・出力で共通。既定はピーク＋RMS）。
    // 変えたらすぐ保存する（設定はアプリが終わるまであるので、画面を作り直しても関数はそのまま使える）。
    // 入力と出力で別々だった頃の inputMeter / outputMeter は、inputMeter を引き継いで消す
    if (! settings.containsKey ("sectionMeter") && settings.containsKey ("inputMeter"))
        settings.setValue ("sectionMeter", settings.getValue ("inputMeter"));
    settings.removeValue ("inputMeter");
    settings.removeValue ("outputMeter");
    ChainView::setSectionMeterKind (settings.getValue ("sectionMeter", "peakRms"));
    ChainView::setSectionMeterChangedCallback ([&s = settings] { saveSectionMeterSettings (s); });

    // プラグインの選択画面で前回選んでいた行と並び（画面を作り直したとき（言語の切り替え）に戻さないよう、起動して最初の 1 回だけ読む）
    static bool browserStateLoaded = false;
    if (! std::exchange (browserStateLoaded, true))
    {
        PluginBrowser::setLastSelection (settings.getValue ("lastBrowserPlugin"), settings.getIntValue ("lastBrowserOffset", 0));
        PluginBrowser::setSortOrder (settings.getIntValue ("browserSort", 0) == (int) PluginBrowser::SortOrder::maker
                                         ? PluginBrowser::SortOrder::maker : PluginBrowser::SortOrder::name);
        PluginBrowser::setEffectsOnly (settings.getBoolValue ("browserEffectsOnly", true));
    }

    // メーターの fps（表示メニュー。既定は 20）
    ChainView::setMeterFramesPerSecond (settings.getIntValue ("meterFps", PluginActivityView::defaultFramesPerSecond));

    // 新しく足したプラグインのメーター（表示メニューで選ぶ）
    {
        const int savedView = settings.getIntValue ("defaultMeterView", (int) PluginChain::Plugin::View::levelBars);
        PluginChain::setDefaultView (PluginChain::Plugin::isValidView (savedView) ? (PluginChain::Plugin::View) savedView
                                                                                   : PluginChain::Plugin::View::levelBars);
    }

    statusLabel.setFont (juce::FontOptions (Theme::popupTextSize)); // ステータスバーの文字は本文より少し小さく
    addAndMakeVisible (statusLabel);
    addChildComponent (overloadBanner); // 処理が間に合っていないときだけ出す

    viewport.setViewedComponent (&chainView, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    addChildComponent (loadingOverlay); // 読み込み中だけ、いちばん上に出す（updateLoadingOverlay）

    refreshEndpoints();
    updateBypassButton();
    rebuildRackChips();
    engine.addChangeListener (this);
    updateLoadingOverlay();

    setSize (1000, 800);
    startTimerHz (timerHz);
}

//==============================================================================
// プリセット
void MainComponent::refreshPresets()
{
    if (! presets.contains (currentPresetId))
        currentPresetId = {}; // 管理の画面で消された

    updatePresetModified();
    settings.setValue ("currentPreset", currentPresetId);
}

namespace
{
// 比べる構成: 並び・ON/OFF・ゲイン・常に処理する・アプリ用ラックなど。
// プラグインの設定データ（state）は比べない（PluginEditWatcher の知らせで見る）。
// プラグインの画面の位置とバイパス、行の表示とエディターの画面の静止画も、変わっても「変更」とみなさない（バイパスはプリセットに含めない）
std::unique_ptr<juce::XmlElement> comparableState (const RackEngine& engine)
{
    auto xml = engine.saveState().createXml();
    if (xml == nullptr)
        return nullptr;

    std::function<void (juce::XmlElement&)> strip = [&strip] (juce::XmlElement& element)
    {
        element.removeAttribute ("state");
        element.removeAttribute ("editorBounds");
        element.removeAttribute ("bypassAll");
        element.removeAttribute ("view");     // 行の表示（見た目だけ）
        element.removeAttribute ("snapshot"); // エディターの画面（開いている間は撮り直すので）
        for (auto* child : element.getChildIterator())
            strip (*child);
    };
    strip (*xml);
    return xml;
}

// メインのラックとアプリ用ラックにある、読み込めているプラグインすべて
std::set<std::pair<int, juce::AudioPluginInstance*>> allPluginInstances (RackEngine& engine)
{
    std::set<std::pair<int, juce::AudioPluginInstance*>> instances;
    auto add = [&instances] (PluginChain& chain)
    {
        for (const auto& plugin : chain.getPlugins())
            if (plugin->instance != nullptr)
                instances.insert ({ plugin->id, plugin->instance.get() });
    };
    add (engine.getChain());
    for (const auto& rack : engine.getAppRacks())
        add (rack->getChain());
    return instances;
}
} // namespace

MainComponent::PluginEditWatcher::~PluginEditWatcher() = default;

void MainComponent::PluginEditWatcher::sync (RackEngine& engine)
{
    // 新しく増えたプラグインに知らせを頼む。なくなったものは解放済みなので何もしない
    const auto current = allPluginInstances (engine);
    for (const auto& entry : current)
        if (watched.count (entry) == 0)
            entry.second->addListener (this);
    watched = current;
}

void MainComponent::PluginEditWatcher::reset (int ignoreMilliseconds)
{
    ignoreUntilMs = juce::Time::getMillisecondCounter() + (juce::uint32) ignoreMilliseconds;
    dirty = false;
}

void MainComponent::PluginEditWatcher::detachAll (RackEngine& engine)
{
    for (const auto& entry : allPluginInstances (engine))
        if (watched.count (entry) != 0)
            entry.second->removeListener (this);
    watched.clear();
}

void MainComponent::takePresetBaseline()
{
    presetBaseline = currentPresetId.isNotEmpty() ? comparableState (engine) : nullptr;
    editWatcher.sync (engine);
    editWatcher.reset (1000); // 読み込みの直後にプラグインから届く知らせは変更と数えない
    presetModified = false;
    updateWindowTitle();
}

void MainComponent::updatePresetModified()
{
    // 読み込み終わってから基準を取る間（baselineAfterLoad）は、基準がまだないだけなので「変更なし」のまま
    // （ここで「変更あり」にすると、読み込み中に落ちた・終了したとき、それが保存されて次の起動でもずっと「変更あり」になる）
    if (baselineAfterLoad)
    {
        presetModified = false;
        updateWindowTitle();
        return;
    }

    editWatcher.sync (engine);

    if (currentPresetId.isEmpty())
        presetModified = false; // 上書きする先がない
    else if (presetBaseline == nullptr || editWatcher.isDirty())
        presetModified = true; // 起動時に変更ありだった、またはプラグインの中で設定を変えた
    else if (const auto now = comparableState (engine))
        presetModified = ! presetBaseline->isEquivalentTo (now.get(), false);

    updateWindowTitle();
}

// タイトルバー: 「PRI System Rack - <今のプリセット>」。保存していない変更があれば最後に「 *」
void MainComponent::updateWindowTitle()
{
    auto* window = findParentComponentOfClass<juce::DocumentWindow>();
    if (window == nullptr)
        return;

    juce::String title (appDisplayName);
    if (currentPresetId.isNotEmpty())
        title << " - " << presets.getName (currentPresetId) << (presetModified ? " *" : "");

    if (window->getName() != title)
        window->setName (title);
}

void MainComponent::loadPreset (const juce::String& id)
{
    const auto state = presets.getState (id);
    if (state == nullptr)
        return;

    // 消えるプラグインやアプリ用ラックを指している画面を先に閉じる
    controller.closeAllEditors();
    rackWindows.clear();
    rackChips.clear();

    // バイパスはプリセットに含めない（今の状態を保つ）
    const bool bypassed = engine.getChain().isBypassAll();
    engine.restoreState (juce::ValueTree::fromXml (*state)); // 行をすぐ戻し、プラグインはこのあと 1 つずつ読み込む
    engine.getChain().setBypassAll (bypassed);

    currentPresetId = id;
    startedLoading(); // 読み込み終わった構成を「変更なし」の基準にする（pluginsLoaded）
    refreshPresets();
    rebuildRackChips();
    settingsDirty = true;
}

void MainComponent::startedLoading()
{
    presetBaseline = nullptr;
    presetModified = false;
    baselineAfterLoad = true;
    updateWindowTitle();
    updateLoadingLock();
}

// プラグインを読み込み終わった（RackEngine::onPluginsLoaded）
void MainComponent::pluginsLoaded()
{
    if (std::exchange (baselineAfterLoad, false))
        takePresetBaseline();
    updateLoadingLock();
    updateStatus();
    settingsDirty = true;

    const auto errors = engine.takeLoadErrors();
    AppLog::write ("plugins loaded" + (errors.isEmpty() ? juce::String() : " | could not load: " + errors.joinIntoString (" / ")));
    if (! errors.isEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Plugins that could not be loaded"),
                                                errors.joinIntoString ("\n"));

    for (auto& callback : std::exchange (afterLoading, {}))
        callback();
}

void MainComponent::whenPluginsLoaded (std::function<void()> callback)
{
    if (engine.isLoadingPlugins())
        afterLoading.push_back (std::move (callback));
    else
        callback();
}

// 読み込み中は画面の操作をすべて止める（メニュー、上の欄、プラグインの行。表示は続ける）
void MainComponent::updateLoadingLock()
{
    const bool locked = engine.isLoadingPlugins();
    if (isEnabled() == ! locked)
        return;
    setEnabled (! locked);
    repaint(); // 自分で描く部品（有効・無効の切り替え、ラックの札など）は、操作できるかが変わっても自分では描き直さないので
}

// ファイルメニューの「新規作成」: まっさらな状態にする（メインのラックのプラグインとアプリ用ラックをすべて外し、
// 入力・出力のゲインを 0 dB に、プリセットは選んでいない状態に）。再生デバイス・バッファ・バイパスなどの設定はそのまま
void MainComponent::confirmNewRack()
{
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, TRANS ("New"),
                                        TRANS ("Remove all plugins and app racks and start from an empty rack?\n"
                                               "Changes that are not saved in a preset will be lost."),
                                        TRANS ("Start new"), TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<MainComponent> (this)] (int result)
                                        {
                                            if (result != 0 && safeThis != nullptr)
                                                safeThis->newRack();
                                        }));
}

void MainComponent::newRack()
{
    // 消えるプラグインやアプリ用ラックを指している画面を先に閉じる
    controller.closeAllEditors();
    rackWindows.clear();
    rackChips.clear();

    // 空の構成を読み込む（バイパスはプリセットと同じく今の状態を保つ）
    const bool bypassed = engine.getChain().isBypassAll();
    juce::ValueTree empty ("PRISystemRack");
    empty.setProperty ("version", 3, nullptr);
    engine.restoreState (empty);
    engine.getChain().setBypassAll (bypassed);

    currentPresetId = {};
    startedLoading();
    refreshPresets();
    rebuildRackChips();
    settingsDirty = true;
}

void MainComponent::saveNewPreset()
{
    const auto suggested = presets.makeUniqueName (TRANS ("Preset %number%").replace ("%number%", juce::String (presets.getPresets().size() + 1)));
    askPresetName (TRANS ("Save as new preset"), suggested, [this] (const juce::String& name)
    {
        if (const auto now = engine.saveState().createXml())
        {
            currentPresetId = presets.add (name, *now);
            takePresetBaseline();
            refreshPresets();
            settings.saveIfNeeded();
        }
    });
}

void MainComponent::overwritePreset()
{
    if (currentPresetId.isEmpty())
        return;

    if (const auto now = engine.saveState().createXml())
        presets.overwrite (currentPresetId, *now);
    takePresetBaseline();
}

void MainComponent::managePresets()
{
    // 並べ替え・名前の変更・削除をしたら、選択欄を作り直す
    PresetManager::show (presets, [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safeThis != nullptr)
            safeThis->refreshPresets();
    });
}

void MainComponent::duplicatePreset()
{
    if (currentPresetId.isEmpty())
        return;

    // 保存してある内容を複製し、複製の方を選ぶ。今の構成はそのままで、未保存の変更があれば
    // それも引き継ぐ（「保存」で複製の方に書ける。元のプリセットは変わらない）
    const auto suggested = presets.makeUniqueName (TRANS ("%name% copy").replace ("%name%", presets.getName (currentPresetId)));
    askPresetName (TRANS ("Duplicate preset"), suggested, [this] (const juce::String& newName)
    {
        const auto newId = presets.duplicate (currentPresetId, newName);
        if (newId.isEmpty())
            return;
        currentPresetId = newId;
        refreshPresets();
        settings.saveIfNeeded();
    });
}

void MainComponent::renamePreset()
{
    if (currentPresetId.isEmpty())
        return;

    askPresetName (TRANS ("Rename preset"), presets.getName (currentPresetId), [this] (const juce::String& newName)
    {
        presets.rename (currentPresetId, newName);
        refreshPresets();
    });
}

void MainComponent::deletePreset()
{
    if (currentPresetId.isEmpty())
        return;

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, TRANS ("Delete preset"),
                                        TRANS ("Delete \"%name%\"? The current chains are not changed.").replace ("%name%", presets.getName (currentPresetId)),
                                        TRANS ("Delete"), TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<MainComponent> (this)] (int result)
                                        {
                                            if (safeThis == nullptr || result == 0)
                                                return;
                                            safeThis->presets.remove (safeThis->currentPresetId);
                                            safeThis->currentPresetId = {};
                                            safeThis->refreshPresets();
                                        }));
}

//==============================================================================
// メニューバー
juce::StringArray MainComponent::getMenuBarNames()
{
    return { TRANS ("File"), TRANS ("View"), TRANS ("Settings"), TRANS ("Help") };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    switch (topLevelMenuIndex)
    {
        case 0:  return createFileMenu();
        case 1:  return createViewMenu();
        case 2:  return createSettingsMenu();
        case 3:  return createHelpMenu();
        default: return {};
    }
}

juce::PopupMenu MainComponent::createSettingsMenu()
{
    juce::PopupMenu menu;
    // プラグイン安全チェック: 足す・置き換える前に、補助プログラムの中で試しに作って、Windows の描画資源を使い切らないかを確かめる
    menu.addItem (TRANS ("Plugin safety check"), true, PluginSafety::isEnabled(), [this]
    {
        PluginSafety::setEnabled (! PluginSafety::isEnabled());
        settings.setValue ("pluginSafetyCheck", PluginSafety::isEnabled());
        settings.saveIfNeeded();
    });

    // 出力の保護（安全リミッター）: NaN・無限大を 0 に、-0.3 dBFS を超えないようにする（既定は ON）
    menu.addItem (TRANS ("Output protection (limiter)"), true, engine.getOutputGuard().isEnabled(), [this]
    {
        engine.getOutputGuard().setEnabled (! engine.getOutputGuard().isEnabled());
        settings.setValue ("outputGuard", engine.getOutputGuard().isEnabled());
        settings.saveIfNeeded();
    });

    // アプリ用ラック（既定は無効）: 無効なら上の欄の「アプリ用ラック」の行を出さず、プリセットのアプリ用ラックは作らずに預かる
    menu.addItem (TRANS ("Use app racks"), true, engine.areAppRacksEnabled(), [this] { setAppRacksEnabled (! engine.areAppRacksEnabled()); });

    menu.addSeparator();

    // × で閉じたとき、終了せずに通知領域にしまう（既定は ON。終了はファイルメニューか通知領域のアイコンの右クリックから）
    menu.addItem (TRANS ("Close to the notification area"), true, settings.getBoolValue ("closeToTray", true), [this]
    {
        settings.setValue ("closeToTray", ! settings.getBoolValue ("closeToTray", true));
        settings.saveIfNeeded();
    });

    // 通知領域のアイコンの絵: メーターと状態 / 静止画（既定。今までのアイコン。バイパス・注意・エラーのときは背景を状態の色にする）
    juce::PopupMenu trayMenu;
    const std::pair<AppTrayIcon::Style, juce::String> trayStyles[] = {
        { AppTrayIcon::Style::metersAndStatus, TRANS ("Meters and status") },
        { AppTrayIcon::Style::still, TRANS ("Still icon") },
    };
    for (const auto& [style, label] : trayStyles)
        trayMenu.addItem (label, true, AppTrayIcon::getStyle() == style, [this, style]
        {
            AppTrayIcon::setStyle (style);
            settings.setValue ("trayIcon", (int) style);
            settings.saveIfNeeded();
        });
    menu.addSubMenu (TRANS ("Notification area icon"), trayMenu);

    // Windows の起動時（ログオン時）に起動する。通知領域にだけ出す（レジストリの Run。設定ファイルには書かない）
    menu.addItem (TRANS ("Start with Windows"), true, AutoStart::isEnabled(), [] { AutoStart::setEnabled (! AutoStart::isEnabled()); });
    return menu;
}

void MainComponent::addTrayMenuItems (juce::PopupMenu& menu)
{
    // プラグインを読み込んでいる間は、どちらも選べない（画面と同じく操作を止める）
    const bool usable = ! engine.isLoadingPlugins();

    // バイパス（上の欄の [バイパス] と同じ）
    menu.addItem (TRANS ("Bypass"), usable, engine.getChain().isBypassAll(), [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safeThis == nullptr)
            return;
        safeThis->engine.getChain().setBypassAll (! safeThis->engine.getChain().isBypassAll());
        safeThis->updateBypassButton();
    });

    // プリセット（ファイルメニューの「プリセットを開く」と同じ）
    juce::PopupMenu presetMenu;
    const auto list = presets.getPresets();
    for (const auto& preset : list)
        presetMenu.addItem (preset.name, true, preset.id == currentPresetId, [safeThis = juce::Component::SafePointer<MainComponent> (this), id = preset.id]
        {
            if (safeThis != nullptr)
                safeThis->loadPreset (id);
        });
    if (list.isEmpty())
        presetMenu.addItem (TRANS ("(no preset)"), false, false, nullptr);
    menu.addSubMenu (TRANS ("Open preset"), presetMenu, usable);
}

juce::PopupMenu MainComponent::createFileMenu()
{
    const bool hasCurrent = currentPresetId.isNotEmpty();
    const auto name = presets.getName (currentPresetId);

    // プリセットを選ぶ（選んだらすぐ読み込む。今の構成が保存されていなくても確認しない）
    juce::PopupMenu openMenu;
    const auto list = presets.getPresets();
    for (const auto& preset : list)
        openMenu.addItem (preset.name, true, preset.id == currentPresetId, [this, id = preset.id] { loadPreset (id); });
    if (list.isEmpty())
        openMenu.addItem (TRANS ("(no preset)"), false, false, nullptr);

    juce::PopupMenu menu;
    menu.addSectionHeader (hasCurrent ? TRANS ("Preset: %name%").replace ("%name%", name) + (presetModified ? TRANS (" (modified)") : juce::String())
                                      : TRANS ("Preset"));
    menu.addItem (TRANS ("New"), [this] { confirmNewRack(); });
    menu.addSubMenu (TRANS ("Open preset"), openMenu);
    menu.addSeparator();
    menu.addItem (TRANS ("Overwrite save"), hasCurrent && presetModified, false, [this] { overwritePreset(); });
    menu.addItem (TRANS ("Save as new preset..."), [this] { saveNewPreset(); });
    menu.addItem (TRANS ("Duplicate..."), hasCurrent, false, [this] { duplicatePreset(); });
    menu.addItem (TRANS ("Rename..."), hasCurrent, false, [this] { renamePreset(); });
    menu.addItem (TRANS ("Delete..."), hasCurrent, false, [this] { deletePreset(); });
    menu.addSeparator();
    menu.addItem (TRANS ("Manage presets..."), [this] { managePresets(); });
    menu.addSeparator();
    menu.addItem (TRANS ("Exit"), [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
    return menu;
}

juce::PopupMenu MainComponent::createViewMenu()
{
    juce::PopupMenu sizeMenu;
    for (const auto scale : Theme::uiScaleChoices)
        sizeMenu.addItem (juce::String (juce::roundToInt (scale * 100.0f)) + "%", true, std::abs (scale - Theme::uiScale) < 0.01f,
                          [this, scale] { chooseUiScale (scale); });

    // 言語名はそれぞれの言語で書く（訳さない）
    juce::PopupMenu languageMenu;
    languageMenu.addItem ("English", true, Language::current() == Language::english, [this] { chooseLanguage (Language::english); });
    languageMenu.addItem (utf8 ("日本語"), true, Language::current() == Language::japanese, [this] { chooseLanguage (Language::japanese); });

    // 新しく足したプラグインのメーター
    juce::PopupMenu meterMenu;
    for (const auto view : PluginActivityView::viewOrder())
    {
        if (view == PluginChain::Plugin::View::none)
            meterMenu.addSeparator();
        meterMenu.addItem (PluginActivityView::viewName (view), true, view == PluginChain::getDefaultView(), [this, view]
        {
            PluginChain::setDefaultView (view);
            settings.setValue ("defaultMeterView", (int) view);
            settings.saveIfNeeded();
        });
    }

    juce::PopupMenu menu;
    menu.addSubMenu (TRANS ("Display size"), sizeMenu);
    menu.addSubMenu (TRANS ("Language"), languageMenu);
    menu.addSeparator();

    // 入力・出力の欄のメーターの種類（メーターの上のホイール・右クリックと同じ。「表示しない」にすると目印がなくなるので、ここからも選べるようにする）
    juce::PopupMenu sectionMeterMenu;
    const auto currentSectionMeter = SectionMeter::fromId (ChainView::getSectionMeterKind());
    for (const auto kind : SectionMeter::order())
    {
        if (SectionMeter::startsGroup (kind))
            sectionMeterMenu.addSeparator();
        sectionMeterMenu.addItem (SectionMeter::name (kind), true, kind == currentSectionMeter, [this, id = SectionMeter::toId (kind)]
        {
            ChainView::setSectionMeterKind (id); // 開いている画面の欄は、次のメーターの更新で切り替わる
            saveSectionMeterSettings (settings);
        });
    }
    // ピークホールド（入力・出力の欄のメーターだけにかかるので、このサブメニューの中に置く。メーターの右クリックのメニューにも同じ項目がある）
    sectionMeterMenu.addSeparator();
    sectionMeterMenu.addItem (TRANS ("Peak hold"), true, ChainView::isPeakHoldEnabled(), [this]
    {
        ChainView::setPeakHoldEnabled (! ChainView::isPeakHoldEnabled());
        saveSectionMeterSettings (settings);
    });
    menu.addSubMenu (TRANS ("Input/output meter"), sectionMeterMenu);
    menu.addSubMenu (TRANS ("Default plugin meter"), meterMenu);

    // メーターの fps: 入力・出力の欄とプラグインの行のメーターを 1 秒に何回更新するか（多いほどなめらかで、描く負荷が増える）
    juce::PopupMenu fpsMenu;
    for (const int fps : PluginActivityView::fpsChoices)
        fpsMenu.addItem (juce::String (fps) + " fps", true, ChainView::getMeterFramesPerSecond() == fps, [this, fps]
        {
            ChainView::setMeterFramesPerSecond (fps);
            settings.setValue ("meterFps", fps);
            settings.saveIfNeeded();
        });
    menu.addSubMenu (TRANS ("Meter fps"), fpsMenu);
    menu.addSeparator();
    menu.addItem (TRANS ("Always on top"), true, AlwaysOnTop::enabled, [this]
    {
        AlwaysOnTop::setEnabled (! AlwaysOnTop::enabled);
        settings.setValue ("alwaysOnTop", AlwaysOnTop::enabled);
        settings.saveIfNeeded();
    });
    return menu;
}

juce::PopupMenu MainComponent::createHelpMenu()
{
    juce::PopupMenu menu;
    menu.addItem (TRANS ("Open log folder"), [this] { openLogFolder(); });
    menu.addSeparator();
    menu.addItem (TRANS ("About PRI System Rack"), [this] { showAbout(); });
    return menu;
}

void MainComponent::openLogFolder()
{
    // APO と設定ツールのログ（%ProgramData%\PRISystemRack\Logs）
    const juce::File folder = juce::File (juce::SystemStats::getEnvironmentVariable ("ProgramData", "C:\\ProgramData"))
                                  .getChildFile ("PRISystemRack").getChildFile ("Logs");
    if (! folder.isDirectory() || ! folder.startAsProcess())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Open log folder"),
                                                TRANS ("The log folder does not exist yet: %path%").replace ("%path%", folder.getFullPathName()));
}

namespace
{
// APO を入れられないデバイス（リモート デスクトップの「リモート オーディオ」など）の説明（ツールチップ、ステータスバー、切り替えの失敗）
juce::String unsupportedDeviceText()
{
    return TRANS ("PRI System Rack cannot be used on this device (for example, the audio of a Remote Desktop session).");
}

// バージョン情報の画面:名前と版、説明の下に、作者の GitHub へのリンク（クリックで既定のブラウザーで開く）
class AboutWindow final : public juce::AlertWindow
{
public:
    AboutWindow (const juce::String& message)
        : juce::AlertWindow (TRANS ("About PRI System Rack"), message, juce::MessageBoxIconType::NoIcon)
    {
        const float fontSize = Theme::bodySize * Theme::effectiveScale();
        link.setFont (juce::FontOptions (fontSize, juce::Font::underlined), false, juce::Justification::centred);
        link.setColour (juce::HyperlinkButton::textColourId, Theme::accent);
        link.setTooltip (url);
        link.setSize (juce::roundToInt (fontSize * 16.0f), juce::roundToInt (fontSize * 1.8f));
        // AlertWindow は、足した部品に名前があると上に左寄せの見出しとして描く。ボタンの文字がそのまま名前になっているので消す
        // （表示する文字は Button の text なので残る）
        link.setName ({});
        addCustomComponent (&link);
        addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey), juce::KeyPress (juce::KeyPress::escapeKey));
    }

    ~AboutWindow() override
    {
        removeCustomComponent (0); // link はこのクラスが持つので、AlertWindow より先に消える。その前に外しておく
    }

private:
    static constexpr const char* url = "https://github.com/pritezza";
    juce::HyperlinkButton link { "github.com/pritezza", juce::URL (url) };
};
} // namespace

void MainComponent::showAbout()
{
    auto* app = juce::JUCEApplication::getInstance();
    // 名前は表示用の「PRI System Rack」（getApplicationName は設定のフォルダー名などに使う「PRISystemRack」なので出さない）
    auto* about = new AboutWindow (juce::String (appDisplayName) + " " + app->getApplicationVersion() + "\n\n"
                                   + TRANS ("Runs VST3 plugins on the sound of Windows playback devices."));
    about->enterModalState (true, nullptr, true);
}

void MainComponent::askPresetName (const juce::String& title, const juce::String& initialName, std::function<void (const juce::String&)> onAccepted)
{
    auto* alert = new juce::AlertWindow (title, TRANS ("Preset name"), juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor ("name", initialName);
    alert->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    if (auto* editor = alert->getTextEditor ("name"))
        editor->selectAll();

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([alert, onAccepted] (int result)
    {
        const auto name = alert->getTextEditorContents ("name").trim();
        if (result == 1 && name.isNotEmpty() && onAccepted != nullptr)
            onAccepted (name);
    }), true);
}

void MainComponent::applyBufferSettings()
{
    const int ms = bufferValue.getValue();
    const bool autoAdjust = bufferAutoToggle.getToggleState();
    bufferValue.setEnabled (! autoAdjust); // 自動のときは今の量を表示するだけ
    engine.setProcessingBuffer (ms, autoAdjust);
    settings.setValue ("processingBufferMs", ms);
    settings.setValue ("processingBufferAuto", autoAdjust);
    settings.saveIfNeeded();
}

MainComponent::~MainComponent()
{
    menuBar.setModel (nullptr);
    stopTimer();
    engine.onPluginsLoaded = nullptr;
    engine.removeChangeListener (this);
    rackWindows.clear();
    controller.closeAllEditors();
    saveSettings();
    editWatcher.detachAll (engine); // プラグインは画面より長く生きる（言語を変えると画面だけ作り直す）
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBackground);

    // 上の欄とラックの境目
    g.setColour (Theme::divider);
    g.fillRect (0, viewport.getY() - 2, getWidth(), 2);

    // ステータスバー（背景と上の境目）
    const auto statusBar = getLocalBounds().removeFromBottom (statusBarHeight);
    g.setColour (Theme::panel);
    g.fillRect (statusBar);
    g.setColour (Theme::divider);
    g.fillRect (statusBar.withHeight (1));
}

void MainComponent::resized()
{
    loadingOverlay.setBounds (getLocalBounds()); // 読み込み中の表示は画面全体（メニューバーも）を覆う

    // 右端のボタンから場所を取り、再生デバイスの選択欄は残りの幅（最大 maxEndpointBoxWidth）を使う。ボタンの幅は文字の長さに合わせる。
    // 見出しの列は文字の幅に合わせる（「再生デバイス」「アプリ用ラック」の長い方 ＋ すき間）
    const juce::FontOptions labelFont (Theme::bodySize);
    const bool showRacks = engine.areAppRacksEnabled(); // 無効なら 3 行目「アプリ用ラック」を出さない（上の欄が 1 行分低くなる）
    int labelWidth = 0;
    for (const auto* label : { &endpointLabel, &racksLabel, &bufferLabel })
        if (label != &racksLabel || showRacks)
            labelWidth = juce::jmax (labelWidth, juce::GlyphArrangement::getStringWidthInt (labelFont, label->getText()));
    labelWidth += 12;

    // 3 行目「アプリ用ラック」の札と「＋ ラックを追加」の位置（見出しの右から左詰めで並べ、入りきらなければ次の行へ折り返す）。
    // 行の数で上の欄の高さが変わるので先に決める
    const int racksWidth = getWidth() - headerPadX * 2 - labelWidth;
    std::vector<juce::Rectangle<int>> rackPlaces; // 札の順、最後が「＋ ラックを追加」
    {
        std::vector<int> widths;
        for (auto& chip : rackChips)
            widths.push_back (juce::jmin (racksWidth, chip->preferredWidth()));
        widths.push_back (juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (RackChip::chipTextSize), addRackButton.getButtonText()) + 24);

        int x = 0, y = 0;
        for (const int width : widths)
        {
            if (x > 0 && x + width > racksWidth)
            {
                x = 0;
                y += RackChip::chipHeight + racksLineGap;
            }
            rackPlaces.emplace_back (x, y, width, RackChip::chipHeight);
            x += width + 6;
        }
    }
    const int racksHeight = juce::jmax (headerRowHeight, rackPlaces.back().getBottom());
    const int headerHeight = headerPadY * 2 + headerRowHeight * 2 + rowGap + (showRacks ? rowGap + racksHeight : 0);

    auto area = getLocalBounds();
    menuBar.setBounds (area.removeFromTop (menuBarHeight));
    auto header = area.removeFromTop (headerHeight).reduced (headerPadX, headerPadY);

    // 1 行目: 見出し、再生デバイスの選択欄（最大 maxEndpointBoxWidth）、そのすぐ右に切り替え [ 有効 | 無効 ]、右端にバイパス。
    // 切り替えの幅は長い方の文言に合わせて固定する（切り替えても並びがずれないように）。どれも常にこの行に置く
    auto top = header.removeFromTop (headerRowHeight);
    const auto slim = [] (juce::Rectangle<int> r) { return r.withSizeKeepingCentre (r.getWidth(), slimBoxHeight); };
    endpointLabel.setBounds (top.removeFromLeft (labelWidth));
    bypassButton.setBounds (slim (top.removeFromRight (Theme::buttonWidth (bypassButton))));
    top.removeFromRight (8);
    const int enableWidth = enableSwitch.getIdealWidth();
    endpointBox.setBounds (slim (top.removeFromLeft (juce::jlimit (0, maxEndpointBoxWidth, top.getWidth() - 10 - enableWidth))));
    top.removeFromLeft (10);
    enableSwitch.setBounds (slim (top.removeFromLeft (enableWidth)));

    // 下端のステータスバー: メッセージ（表示の大きさと言語は「表示」メニュー）
    statusLabel.setBounds (area.removeFromBottom (statusBarHeight).withTrimmedTop (1).reduced (12, 3));
    if (overloadBanner.isVisible())
        overloadBanner.setBounds (area.removeFromBottom (overloadBanner.getHeightForWidth (area.getWidth())));

    // 2 行目「バッファ」: 量、自動、無音時に休止、音声テスト（入力 / 出力）
    header.removeFromTop (rowGap);
    auto bufferRow = header.removeFromTop (headerRowHeight);
    bufferLabel.setBounds (bufferRow.removeFromLeft (labelWidth));
    bufferValue.setBounds (slim (bufferRow.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (labelFont, "200 ms") + 20)));
    bufferRow.removeFromLeft (10);
    bufferAutoToggle.setBounds (bufferRow.removeFromLeft (Theme::toggleWidth (bufferAutoToggle)));
    bufferRow.removeFromLeft (16);
    restToggle.setBounds (bufferRow.removeFromLeft (Theme::toggleWidth (restToggle)));
    bufferRow.removeFromLeft (20);
    testToggles.setBounds (bufferRow.removeFromLeft (testToggles.getIdealWidth()));

    // 3 行目「アプリ用ラック」（プリセットはファイルメニュー）。無効なら出さない（部品は隠してある）
    header.removeFromTop (showRacks ? rowGap : 0);
    auto racksRow = header.removeFromTop (showRacks ? racksHeight : 0);
    racksLabel.setBounds (racksRow.getX(), racksRow.getY(), labelWidth, headerRowHeight);
    // 1 行だけのときは、ほかの行の部品と縦の中心をそろえる
    const auto origin = juce::Point<int> (racksRow.getX() + labelWidth,
                                          racksRow.getY() + (rackPlaces.back().getY() == 0 ? (headerRowHeight - RackChip::chipHeight) / 2 : 0));
    for (size_t i = 0; i < rackChips.size(); ++i)
        rackChips[i]->setBounds (rackPlaces[i] + origin);
    addRackButton.setBounds (rackPlaces.back() + origin);

    area.removeFromTop (2);
    viewport.setBounds (area);
    chainView.setMinimumHeight (viewport.getHeight());
    chainView.setSize (viewport.getMaximumVisibleWidth(), chainView.getHeight());
}

//==============================================================================
void MainComponent::chooseLanguage (const juce::String& code)
{
    if (code == Language::current())
        return;

    settings.setValue ("language", code);
    saveSettings();

    // この画面は作り直されるので、選択の処理が終わってから知らせる
    juce::MessageManager::callAsync ([callback = languageChanged]
    {
        if (callback != nullptr)
            callback();
    });
}

// 表示の大きさを変える。メイン画面とアプリ用ラックのウィンドウを、中身の配置を保ったまま拡大・縮小する
void MainComponent::chooseUiScale (float scale)
{
    if (std::abs (scale - Theme::uiScale) < 0.01f)
        return;

    Theme::uiScale = scale;
    settings.setValue ("displayScale", (double) Theme::uiScale);
    settingsDirty = true;

    // メニューの処理が終わってから大きさを変える（この部品の大きさも変わるため）
    juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safeThis == nullptr)
            return;

        auto* scaled = safeThis->findParentComponentOfClass<ScaledContent>();
        auto* window = safeThis->findParentComponentOfClass<juce::ResizableWindow>();
        if (scaled != nullptr && window != nullptr)
            scaled->rescaleWindow (*window, Theme::effectiveScale(), Theme::minWindowWidth, Theme::minWindowHeight);

        for (auto& [id, rackWindow] : safeThis->rackWindows)
            rackWindow->applyUiScale();
    });
}

//==============================================================================
void MainComponent::rebuildRackChips()
{
    // ラックの顔ぶれが同じなら作り直さず、描き直すだけにする
    const auto& racks = engine.getAppRacks();

    // 機能が無効なら「アプリ用ラック」の行（見出しと「＋ ラックを追加」）を出さない（ラックの札はラックがないので出ない）
    racksLabel.setVisible (engine.areAppRacksEnabled());
    addRackButton.setVisible (engine.areAppRacksEnabled());

    // 上限（RackEngine::maxAppRacks）に達したら「＋ ラックを追加」を押せなくする
    const bool canAdd = (int) racks.size() < RackEngine::maxAppRacks;
    addRackButton.setEnabled (canAdd);
    addRackButton.setTooltip (canAdd ? TRANS ("Capture the audio of specific apps, process it with its own plugins and merge it into the main rack")
                                     : TRANS ("You can add up to %count% app racks").replace ("%count%", juce::String (RackEngine::maxAppRacks)));
    bool same = racks.size() == rackChips.size();
    for (size_t i = 0; same && i < racks.size(); ++i)
        same = rackChips[i]->getRackId() == racks[i]->getId();

    if (! same)
    {
        rackChips.clear();
        for (const auto& rack : racks)
        {
            auto chip = std::make_unique<RackChip> (*rack);
            const int id = rack->getId();
            chip->onOpen = [this, id] { openAppRack (id); };
            chip->onDelete = [this, id] { confirmDeleteAppRack (id); };
            addAndMakeVisible (*chip);
            rackChips.push_back (std::move (chip));
        }
    }

    // 消えたラックのウィンドウを閉じる
    for (auto it = rackWindows.begin(); it != rackWindows.end();)
    {
        if (engine.findAppRack (it->first) == nullptr)
            it = rackWindows.erase (it);
        else
            ++it;
    }

    resized();
    for (auto& chip : rackChips)
        chip->repaint();
}

void MainComponent::setAppRacksEnabled (bool shouldBeEnabled)
{
    const bool hadRacks = ! engine.getAppRacks().empty();
    const bool wasModified = presetModified;

    // 消えるラックを指している画面を先に閉じる
    rackWindows.clear();
    rackChips.clear();

    engine.setAppRacksEnabled (shouldBeEnabled);
    settings.setValue ("appRacks", shouldBeEnabled);
    settings.saveIfNeeded();
    settingsDirty = true;

    // 切り替えただけでは「変更あり」にしない（預かった中身と作り直したラックとで、書き出しが少し違うことがあるため、基準を取り直す）
    if (! wasModified && currentPresetId.isNotEmpty())
    {
        if (engine.isLoadingPlugins())
            baselineAfterLoad = true;
        else
            takePresetBaseline();
    }

    updateLoadingLock(); // 有効にしたら、預かっていたラックのプラグインを読み込む間は操作を止める
    updateStatus();
    rebuildRackChips();

    // 上の欄の高さが 1 行分変わって下の画面ごとずれるので、全体を描き直す（部品ごとの描き直しだけだと、
    // 消えた 3 行目の札とボタンの縁が残ることがあった）
    repaint();

    if (! shouldBeEnabled && hadRacks)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Use app racks"),
                                                TRANS ("The app racks were stopped but not deleted. They stay in the current configuration and in presets, "
                                                       "and come back when you turn app racks on again."));
}

void MainComponent::addAppRack()
{
    if (auto* rack = engine.addAppRack())
        openAppRack (rack->getId());
}

void MainComponent::openAppRack (int rackId)
{
    auto* rack = engine.findAppRack (rackId);
    if (rack == nullptr)
        return;

    auto& window = rackWindows[rackId];
    if (window == nullptr)
        window = std::make_unique<AppRackWindow> (*rack, [this] (int id) { deleteAppRack (id); });
    window->showAndFocus();
}

// 上の欄の札の右クリック →「削除」。ラックの画面の [ラックを削除] と同じ確認を出す
void MainComponent::confirmDeleteAppRack (int rackId)
{
    const auto* rack = engine.findAppRack (rackId);
    if (rack == nullptr)
        return;

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, TRANS ("Delete rack"),
                                        TRANS ("Delete %name% and its plugins?").replace ("%name%", rack->getName()),
                                        TRANS ("Delete"), TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<MainComponent> (this), rackId] (int result)
                                        {
                                            if (result != 0 && safeThis != nullptr)
                                                safeThis->deleteAppRack (rackId);
                                        }));
}

void MainComponent::deleteAppRack (int rackId)
{
    rackWindows.erase (rackId); // プラグインの画面ごと、ラックより先に閉じる
    engine.removeAppRack (rackId);
    rebuildRackChips(); // 消えたラックを指すボタンを、変更の通知を待たずに外す
}

//==============================================================================
void MainComponent::refreshEndpoints()
{
    endpoints = pri::host::ListRenderEndpoints();

    const auto current = engine.getEndpointGuid().isNotEmpty() ? engine.getEndpointGuid()
                                                               : settings.getValue ("endpointGuid");

    endpointBox.clear (juce::dontSendNotification);
    int selectedId = 0;

    for (size_t i = 0; i < endpoints.size(); ++i)
    {
        const auto& endpoint = endpoints[i];
        auto name = juce::String (endpoint.friendlyName.c_str());
        if (endpoint.apoInstalled)
            name << "  [APO]";
        else if (! endpoint.canHaveApo)
            name << "  [" << TRANS ("Not supported") << "]"; // リモート デスクトップの音声など、APO を入れられないもの
        else if (endpoint.otherApoPresent)
            name << "  [" << TRANS ("Other APO") << "]"; // 特定の製品の名前は出さない
        const int itemId = (int) i + 1;
        endpointBox.addItem (name, itemId);

        if (juce::String (endpoint.guid.c_str()).equalsIgnoreCase (current))
            selectedId = itemId;
    }

    // 選んでいたデバイスが、サンプルレートの切り替えで別のエンドポイントに入れ替わった。AG06 の Yamaha Steinberg USB Driver などは
    // レートごとに別のエンドポイント（別の GUID）を作るので、ASIO のアプリ（Cubase など）がレートを変えると、同じ「Line (AG06/AG03)」が
    // 別の GUID で現れ、元の GUID は「ない」になる（2026-11-11 に確認。Cubase を閉じてもレートは戻らない）。
    // 同じ機器の同じ出力が 1 つだけ使えるなら、そちらに移る（APO との共有メモリの名前も GUID ごとなので、移らないとつながらない）
    if (selectedId == 0 && current.isNotEmpty())
    {
        if (const auto successor = pri::host::FindActiveSuccessor (current.toWideCharPointer()))
        {
            for (size_t i = 0; i < endpoints.size() && selectedId == 0; ++i)
                if (endpoints[i].guid == successor->guid)
                    selectedId = (int) i + 1;

            if (selectedId != 0)
            {
                const juce::String name (successor->friendlyName.c_str());
                setupNotice = successor->apoInstalled
                                  ? TRANS ("The sample rate of %device% was changed (by an ASIO application, for example), so processing moved to the device for the new rate")
                                        .replace ("%device%", name)
                                  : TRANS ("The sample rate of %device% was changed (by an ASIO application, for example). PRI System Rack is not enabled for the new rate yet. Press [Enable]")
                                        .replace ("%device%", name);
                setupNoticeColour = successor->apoInstalled ? juce::Colours::lightgrey : warningColour;
                setupNoticeUntilMs = juce::Time::getMillisecondCounter() + 15000;
            }
        }
    }

    // 選んでいたデバイスが今はない（USB の機器を抜いた、無効にしたなど）。
    // 別のデバイスに勝手に切り替えず、「接続されていません」と出して選んだままにする（戻ってきたらそのまま使う）
    if (selectedId == 0 && current.isNotEmpty())
    {
        const auto name = engine.getEndpointName().isNotEmpty() ? engine.getEndpointName() : settings.getValue ("endpointName");
        engine.setEndpoint (name, current); // 起動直後でもこのデバイスを選んだままにする（APO との接続はデバイスが戻るまで待つ）
        selectedEndpointMissing = true;
        endpointBox.setSelectedId (0, juce::dontSendNotification);
        endpointBox.setText (TRANS ("%name% (not connected)").replace ("%name%", name.isNotEmpty() ? name : TRANS ("Playback device")),
                             juce::dontSendNotification);
        updateEnableState();
        return;
    }

    // 初めて使うときは、APO が入っている最初のデバイスを選ぶ
    if (selectedId == 0)
        for (size_t i = 0; i < endpoints.size() && selectedId == 0; ++i)
            if (endpoints[i].apoInstalled)
                selectedId = (int) i + 1;

    // 抜かれていたデバイスが戻ってきたら、APO との接続と無音のストリームを作り直す（同じデバイスだと setEndpoint は何もしないため）
    if (std::exchange (selectedEndpointMissing, false) && selectedId != 0)
        engine.setEndpoint ({}, {});

    endpointBox.setSelectedId (selectedId, juce::dontSendNotification);
    endpointChosen();
}

// 無効にしたときに戻る、入れる前の状態（今の FxProperties からは、控えに移した APO は分からないので控えを見る）。
// 特定の製品の名前は画面に出さないので、APO の名前（backupApo）は使わず「他の APO」か「APO なし」とだけ出す
juce::String MainComponent::previousApoName (const pri::host::RenderEndpoint& endpoint) const
{
    return endpoint.backupHasApo ? TRANS ("Other APO") : TRANS ("no APO");
}

void MainComponent::endpointChosen()
{
    const int index = endpointBox.getSelectedId() - 1;

    updateEnableState();

    if (index < 0 || index >= (int) endpoints.size())
    {
        engine.setEndpoint ({}, {});
        return;
    }

    const auto& endpoint = endpoints[(size_t) index];
    selectedEndpointMissing = false; // 一覧にあるデバイスを選んだ
    engine.setEndpoint (endpoint.friendlyName.c_str(), endpoint.guid.c_str());
    settingsDirty = true;
}

const pri::host::RenderEndpoint* MainComponent::selectedEndpoint() const
{
    const int index = endpointBox.getSelectedId() - 1;
    return index >= 0 && index < (int) endpoints.size() ? &endpoints[(size_t) index] : nullptr;
}

void MainComponent::updateEnableState()
{
    const auto* endpoint = selectedEndpoint();
    const bool enabled = endpoint != nullptr && endpoint->apoInstalled;

    enableSwitch.setState (enabled);
    // 切り替えている間は押せない（ステータスバーに「切り替え中...」）。APO を入れられないデバイスでも押せない
    enableSwitch.setEnabled (endpoint != nullptr && (endpoint->canHaveApo || enabled) && ! setupRunning);
    if (endpoint == nullptr)
    {
        enableSwitch.currentTooltip = enableSwitch.actionTooltip = {};
    }
    else if (! endpoint->canHaveApo && ! enabled)
    {
        enableSwitch.currentTooltip = enableSwitch.actionTooltip = unsupportedDeviceText();
    }
    else if (enabled)
    {
        enableSwitch.currentTooltip = TRANS ("Enabled: the sound of this device is processed by PRI System Rack.");
        enableSwitch.actionTooltip = TRANS ("Removes PRI System Rack from this device and restores its previous state (%previous%). "
                                            "Needs administrator permission, and Windows sound stops for a few seconds.")
                                         .replace ("%previous%", previousApoName (*endpoint));
    }
    else
    {
        enableSwitch.currentTooltip = TRANS ("Disabled: PRI System Rack does not process the sound of this device.");
        enableSwitch.actionTooltip = TRANS ("Processes the sound of this device with PRI System Rack. Needs administrator permission, and Windows sound stops "
                                            "for a few seconds (if another APO is installed, it is removed and saved, and restored when you disable this).");
    }

    // 有効にしている間（と切り替えている間）は、再生デバイスを選び直せなくする。一度に処理できるのは 1 つのデバイスだけで、
    // 選び直すと今のデバイスの処理が黙って止まる（APO は素通しになる）ため。別のデバイスで使うときは先に［無効］にする。
    // 選んでいたデバイスが接続されていないとき（endpoint がない）は選び直せる
    const bool lockDevice = enabled || setupRunning;
    endpointBox.setEnabled (! lockDevice);
    endpointBox.setTooltip (lockDevice ? TRANS ("The playback device cannot be changed while it is enabled. To use another device, press [Disable] first.")
                                       : juce::String());

    // 有効でなければ処理されないので、バイパスは押せなくし、プラグインはすべて休止と表示する
    bypassButton.setEnabled (enabled);
    PluginChain::setDeviceEnabled (enabled);
    updateStatus();
}

void MainComponent::updateBypassButton()
{
    const bool bypassed = engine.getChain().isBypassAll();
    bypassButton.setToggleState (bypassed, juce::dontSendNotification);
    bypassButton.setColour (juce::TextButton::buttonColourId, Theme::button);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, Theme::bypassOn);
    bypassButton.setColour (juce::TextButton::textColourOffId, Theme::text);
    bypassButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    bypassButton.setColour (juce::ComboBox::outlineColourId, bypassed ? warningColour : Theme::buttonBorder);
}

void MainComponent::enableClicked()
{
    const auto* endpoint = selectedEndpoint();

    if (endpoint == nullptr || setupRunning)
        return;

    const juce::String name (endpoint->friendlyName.c_str());
    const juce::String guid (endpoint->guid.c_str());

    if (pri::host::FindSetupTool().empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot switch"),
                                                TRANS ("PRISystemRackSetup.exe was not found next to the application."));
        return;
    }

    juce::String message;
    juce::String arguments;
    juce::String title, okText;
    juce::String successNotice; // 終わったらステータスバーに出す

    // 同じ機器の同じ出力の、ほかのサンプルレートのエンドポイント（今ないものも含む）もまとめて切り替える。
    // 有効にしておけば、ASIO のアプリがレートを変えたときも、管理者の権限なしにそのまま使える（refreshEndpoints で移る）。
    // Setup ツールには、選んだデバイスを先頭に、残りを後ろに並べて渡す
    juce::String siblingGuids;
    bool siblingHasOtherApo = false;
    for (const auto& sibling : pri::host::FindSiblingEndpoints (endpoint->guid))
    {
        if (endpoint->apoInstalled && ! sibling.apoInstalled && ! sibling.backupPresent)
            continue; // 外すとき: PRISystemRack を入れていないものは触らない
        siblingGuids << " " << juce::String (sibling.guid.c_str());
        siblingHasOtherApo = siblingHasOtherApo || (! endpoint->apoInstalled && sibling.otherApoPresent);
    }

    if (endpoint->apoInstalled)
    {
        title = TRANS ("Disable PRI System Rack");
        okText = TRANS ("Disable");
        const auto previous = previousApoName (*endpoint);
        message = TRANS ("Removes PRI System Rack from %device% and restores its previous state (%previous%).")
                      .replace ("%device%", name)
                      .replace ("%previous%", previous);
        successNotice = TRANS ("Disabled on %device% (restored %previous%)").replace ("%device%", name).replace ("%previous%", previous);
        arguments = "detach " + guid + siblingGuids;
    }
    else
    {
        if (! pri::host::IsApoRegistered())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot switch"),
                                                    TRANS ("The PRI System Rack APO is not installed.\nDuring development, install it with %script%.")
                                                        .replace ("%script%", devInstallScript));
            return;
        }

        title = TRANS ("Enable PRI System Rack");
        okText = TRANS ("Enable");
        message = TRANS ("Uses PRI System Rack on %device%.").replace ("%device%", name);
        successNotice = TRANS ("Enabled on %device%").replace ("%device%", name);
        if (endpoint->otherApoPresent || siblingHasOtherApo)
        {
            message += "\n" + TRANS ("The other APO currently installed on this device will be removed and its settings saved (you can restore it by disabling).");
            successNotice += " " + TRANS ("(the other APO is saved and comes back when you disable)");
        }
        arguments = "attach " + guid + siblingGuids;
    }

    // UAC の確認が出るなら、ここでは確認を出さずに進める（UAC の確認がその役目。そこで取りやめれば何も変わらない）。
    // UAC が出ない（すでに管理者として動いている、UAC が「通知しない」など）ときは、押しただけで音が止まらないよう、ここで確認する
    if (pri::host::WillShowUacPrompt())
    {
        runSetup (arguments, successNotice);
        return;
    }

    message += "\n\n" + TRANS ("Windows Audio will be restarted, so Windows sound will stop for a few seconds.");

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, title, message,
                                        okText, TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<MainComponent> (this), arguments, successNotice] (int result)
                                        {
                                            if (result != 0 && safeThis != nullptr)
                                                safeThis->runSetup (arguments, successNotice);
                                        }));
}

void MainComponent::runSetup (const juce::String& arguments, const juce::String& successNotice)
{
    setupRunning = true;
    setupNoticeUntilMs = 0;
    updateEnableState();

    // UAC と音声サービスの再起動を待つ間、画面を止めないように別スレッドで実行する
    juce::Thread::launch ([safeThis = juce::Component::SafePointer<MainComponent> (this), arguments, successNotice]
    {
        const int result = pri::host::RunSetupElevated (arguments.toWideCharPointer());

        juce::MessageManager::callAsync ([safeThis, result, successNotice, arguments]
        {
            if (safeThis == nullptr)
                return;

            safeThis->setupRunning = false;
            AppLog::write ("setup " + arguments + " -> exit code " + juce::String (result)
                           + (result == -1 ? " (cancelled)" : result == 3 ? " (some other sample rates failed)" : juce::String()));

            // 結果をステータスバーにしばらく出す（取りやめたときも、何も変わっていないことを伝える）
            if (result == 0 || result == -1)
            {
                safeThis->setupNotice = result == 0 ? successNotice : TRANS ("Switching was cancelled. Nothing was changed");
                safeThis->setupNoticeColour = juce::Colours::lightgrey;
                safeThis->setupNoticeUntilMs = juce::Time::getMillisecondCounter() + 10000;
            }

            // 3: 選んだデバイスは切り替えたが、同じ出力のほかのサンプルレートのエンドポイントのどれかで失敗した（Setup ツールの終了コード）。
            // 今の音には関係ないので、ダイアログではなく注意の色で長めに出す
            if (result == 3)
            {
                safeThis->setupNotice = successNotice + "  " + TRANS ("However, it could not be switched for some other sample rates of this device, so sound at those rates may not be processed (see setup.log)");
                safeThis->setupNoticeColour = warningColour;
                safeThis->setupNoticeUntilMs = juce::Time::getMillisecondCounter() + 30000;
            }

            safeThis->refreshEndpoints();

            if (result == -1 || result == 3)
                return; // UAC で取りやめた、またはほかのレートだけ失敗した（上で出した）

            // 4: Windows の再生デバイスの登録にないデバイス（リモート デスクトップの音声など。一覧を読み直す前に選ばれたとき）
            if (result == 4)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot switch"),
                                                        unsupportedDeviceText());
            else if (result != 0)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Switching failed"),
                                                        TRANS ("Exit code %code%\nSee %path% for details.")
                                                            .replace ("%code%", juce::String (result))
                                                            .replace ("%path%", setupLogPath));
        });
    });
}

void MainComponent::updateStatus()
{
    // kind: 状態の種類（英語の固定の名前）。種類が変わったときだけ app.log に書く（処理中の数値などは毎回変わるので、文章では比べない）
    auto setStatus = [this] (const juce::String& kind, const juce::String& text, juce::Colour colour)
    {
        if (kind != loggedStatusKind)
        {
            loggedStatusKind = kind;
            AppLog::write ("status: " + kind + " | device: " + engine.getEndpointName() + " | " + text);
        }
        statusLabel.setText (utf8 ("\xe2\x97\x8f ") + text, juce::dontSendNotification); // ●
        statusLabel.setTooltip (text); // 幅が足りず省略されたときに全文を見られるように
        statusLabel.setColour (juce::Label::textColourId, colour);
    };

    updateLoadingOverlay();

    // プラグインを 1 つずつ読み込んでいる間（音は素通し）
    if (engine.isLoadingPlugins())
    {
        setStatus ("loading plugins", TRANS ("Loading plugins... (%done% / %total%)  The sound passes through without the plugins until all are loaded")
                       .replace ("%done%", juce::String (engine.getLoadedCount()))
                       .replace ("%total%", juce::String (engine.getLoadTotal())),
                   juce::Colours::lightgrey);
        return;
    }

    if (engine.getEndpointGuid().isEmpty())
    {
        setStatus ("no device chosen", TRANS ("Choose a playback device (devices marked [APO] can be used)"), juce::Colours::lightgrey);
        return;
    }

    if (setupRunning)
    {
        setStatus ("switching", TRANS ("Switching... When you allow it, Windows Audio restarts and Windows sound stops for a few seconds"), juce::Colours::lightgrey);
        return;
    }

    // 切り替えの結果（しばらく出す）
    if (setupNoticeUntilMs != 0 && (juce::int32) (setupNoticeUntilMs - juce::Time::getMillisecondCounter()) > 0)
    {
        setStatus ("notice: " + setupNotice, setupNotice, setupNoticeColour);
        return;
    }
    setupNoticeUntilMs = 0;

    const auto* endpoint = selectedEndpoint();
    if (endpoint == nullptr)
    {
        setStatus ("device not connected", TRANS ("The playback device is not connected. Processing resumes when it is connected again"), warningColour);
        return;
    }

    if (! endpoint->apoInstalled && ! endpoint->canHaveApo)
    {
        setStatus ("device not supported", unsupportedDeviceText() + " " + TRANS ("Choose another playback device."), warningColour);
        return;
    }

    if (! endpoint->apoInstalled)
    {
        setStatus ("disabled on this device", TRANS ("Disabled on this device. Click \"Enabled\" next to the device to process its sound"), juce::Colours::lightgrey);
        return;
    }

    const auto stats = engine.getLinkStats();

    if (! stats.connected)
    {
        setStatus ("waiting for the APO (error " + juce::String ((juce::int64) stats.lastConnectError) + ")", TRANS ("Waiting for the APO: %reason%").replace ("%reason%", describeConnectError (stats.lastConnectError)),
                   warningColour);
        return;
    }

    if (engine.getChain().isBypassAll())
    {
        setStatus ("bypass", TRANS ("Bypass is on: plugins are bypassed (gain is still applied)"), warningColour);
        return;
    }

    if (! stats.apoActive)
    {
        setStatus ("connected, no audio yet", TRANS ("Connected to the APO. Processing starts when audio plays on this device"), juce::Colours::lightgrey);
        return;
    }

    const auto& chain = engine.getChain();
    auto text = TRANS ("Processing via APO  %format%  |  Round trip avg %avg% %us%, max %max% %us%  |  Timeouts %timeouts%  |  Plugins on %on% / %total%")
                    .replace ("%format%", engine.describeFormat())
                    .replace ("%avg%", juce::String (juce::roundToInt (stats.roundTripMicrosAverage)))
                    .replace ("%max%", juce::String (juce::roundToInt (stats.roundTripMicrosMax)))
                    .replace ("%us%", utf8 ("\xc2\xb5s")) // µs
                    .replace ("%timeouts%", juce::String ((juce::int64) stats.apoTimedOut))
                    .replace ("%on%", juce::String (chain.countEnabled()))
                    .replace ("%total%", juce::String ((int) chain.getPlugins().size()));

    // 処理バッファを使っているときは、間に合わなかった回数も出す（増えるならバッファを大きくする）
    if (engine.isProcessingBufferOn())
        text << "  |  " << TRANS ("Buffer underruns %count%").replace ("%count%", juce::String ((juce::int64) engine.getBufferUnderruns()));

    // 出力の保護（安全リミッター）が働いたら、その回数を出す（起動してからの合計。不正な値（NaN・無限大）を 0 にしたときはその数も）
    const auto& guard = engine.getOutputGuard();
    if (guard.getLimitCount() > 0)
        text << "  |  " << TRANS ("Output protection %count%").replace ("%count%", juce::String ((juce::int64) guard.getLimitCount()));
    if (guard.getInvalidCount() > 0)
        text << "  |  " << TRANS ("Invalid samples %count%").replace ("%count%", juce::String ((juce::int64) guard.getInvalidCount()));

    setStatus (guard.getInvalidCount() > 0 ? "processing (invalid samples were zeroed)" : "processing", text,
               guard.getInvalidCount() > 0 ? warningColour : okColour);
}

juce::String MainComponent::describeWaiting() const
{
    // 読み込み中・デバイスを選んでいない・切り替え中は、待っているのではない（ステータスバーも灰色）
    if (engine.isLoadingPlugins() || engine.getEndpointGuid().isEmpty() || setupRunning)
        return {};

    // 選んでいたデバイスが今はない（USB の機器を抜いた、無効にしたなど）。デバイスの［有効］も下がるので、
    // 通知領域のアイコンからは「無効」と見分けられない。そのためここで知らせる
    const auto* endpoint = selectedEndpoint();
    if (endpoint == nullptr)
        return TRANS ("The playback device is not connected. Processing resumes when it is connected again");

    if (! endpoint->apoInstalled)
        return {}; // 無効にしてある（利用者がそうした）

    const auto stats = engine.getLinkStats();
    if (! stats.connected)
        return TRANS ("Waiting for the APO: %reason%").replace ("%reason%", describeConnectError (stats.lastConnectError));
    return {};
}

//==============================================================================
void MainComponent::updateOverloadBanner()
{
    const auto& overload = engine.checkOverload();
    const bool show = overload.active && ! engine.isLoadingPlugins() && engine.getLinkStats().connected;

    if (show != loggedOverload)
    {
        loggedOverload = show;
        AppLog::write (show ? "overload started: " + overload.describeTitle() + " | " + overload.describeDetail()
                            : juce::String ("overload ended"));
    }

    bool changed = show != overloadBanner.isVisible();
    if (show)
        changed |= overloadBanner.setText (overload.describeTitle(), overload.describeDetail());
    overloadBanner.setVisible (show);
    if (changed)
        resized();
}

bool MainComponent::OverloadBanner::setText (const juce::String& newTitle, const juce::String& newDetail)
{
    if (newTitle == title && newDetail == detail)
        return false;
    title = newTitle;
    detail = newDetail;
    repaint();
    return true;
}

juce::TextLayout MainComponent::OverloadBanner::layoutFor (float width) const
{
    juce::AttributedString text;
    text.setWordWrap (juce::AttributedString::byWord);
    text.append (utf8 ("\xe2\x9a\xa0 ") + title, juce::FontOptions (Theme::bodySize, juce::Font::bold), Theme::error); // ⚠
    if (detail.isNotEmpty())
        text.append ("\n" + detail, juce::FontOptions (Theme::smallSize), Theme::text);

    juce::TextLayout layout;
    layout.createLayout (text, juce::jmax (1.0f, width));
    return layout;
}

int MainComponent::OverloadBanner::getHeightForWidth (int width) const
{
    return (int) std::ceil (layoutFor ((float) width - 28.0f).getHeight()) + 14;
}

void MainComponent::OverloadBanner::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff4a2326)); // 暗い赤
    g.setColour (Theme::error);
    g.fillRect (0, 0, getWidth(), 1);
    g.fillRect (0, 0, 4, getHeight()); // 左の縦線
    layoutFor ((float) getWidth() - 28.0f).draw (g, getLocalBounds().toFloat().reduced (16.0f, 7.0f).withTrimmedRight (-4.0f));
}

void MainComponent::timerCallback()
{
    // メーターは ChainView が自分のタイマー（メーターの fps）で更新する

    // 再生デバイスが変わったという通知が来ていたら、通知が途切れてから一覧を読み直す（一覧を開いている間は待つ）
    if (const auto changes = deviceWatcher.GetChangeCount(); changes != seenDeviceChanges)
    {
        seenDeviceChanges = changes;
        lastDeviceChangeMs = juce::Time::getMillisecondCounter();
        deviceListStale = true;
    }
    if (deviceListStale && ! endpointBox.isPopupActive() && ! setupRunning
        && juce::Time::getMillisecondCounter() - lastDeviceChangeMs >= 300)
    {
        deviceListStale = false;
        refreshEndpoints();
    }

    // 手動で変えた処理バッファの量は、変更が 0.3 秒止まってから反映する
    if (pendingBufferMs >= 0 && juce::Time::getMillisecondCounter() - bufferChangedAtMs >= 300)
    {
        pendingBufferMs = -1;
        applyBufferSettings();
    }

    // 自動のときは、自動で決まった今の量を表示する
    if (bufferAutoToggle.getToggleState() && timerTicks % (timerHz / 2) == 0 && engine.isProcessingBufferOn())
        bufferValue.setValue (juce::roundToInt (engine.getProcessingBufferCurrentMs()));

    if (++timerTicks % (timerHz / 2) == 0)
    {
        updateStatus();
        updateOverloadBanner();
        for (auto& chip : rackChips)
            chip->repaint(); // 状態の点
    }

    // プラグインの画面の中で変えた設定は知らせが来ないので、1 秒ごとに比べて [保存] を押せるかを決める（読み込み中は比べない）
    if (timerTicks % timerHz == 0 && ! engine.isLoadingPlugins())
        updatePresetModified();

    // 落ちたときの記録の登録を、プラグインに差し替えられていたら戻す（5 秒ごと）
    if (timerTicks % (timerHz * 5) == 0)
        CrashLog::reinstall();

    // 構成が変わったら 2 秒後に、それ以外も 30 秒ごとに保存する（プラグイン内の設定変更は通知がないため）
    if ((settingsDirty && timerTicks % (timerHz * 2) == 0) || timerTicks % (timerHz * 30) == 0)
        saveSettings();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    settingsDirty = true;
    updateBypassButton();
    updateStatus(); // 読み込み中は進み具合
    rebuildRackChips();
    updateLoadingOverlay(); // ラックの札を作り直したあとで、いちばん上に戻す
}

void MainComponent::updateLoadingOverlay()
{
    // 読み込むものがないとき（0 個）は一瞬で終わるので出さない
    const bool show = engine.isLoadingPlugins() && engine.getLoadTotal() > 0;
    if (show)
    {
        loadingOverlay.setProgress (engine.getLoadedCount(), engine.getLoadTotal(), engine.getNextLoadingName());
        loadingOverlay.toFront (false);
    }
    loadingOverlay.setVisible (show);
}

bool MainComponent::LoadingOverlay::setProgress (int newDone, int newTotal, const juce::String& newNext)
{
    if (newDone == done && newTotal == total && newNext == next)
        return false;
    done = newDone;
    total = newTotal;
    next = newNext;
    repaint();
    return true;
}

void MainComponent::LoadingOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.45f)); // 後ろの画面を暗くする

    // 中央のカード: 見出し、何個目か、進み具合のバー、次に読み込むプラグイン、音のこと
    const int cardWidth = juce::jmin (440, getWidth() - 40);
    auto card = getLocalBounds().withSizeKeepingCentre (cardWidth, 146);
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (card.toFloat(), 8.0f);
    g.setColour (Theme::cardBorder);
    g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 8.0f, 1.0f);

    auto area = card.reduced (22, 18);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (Theme::titleSize, juce::Font::bold));
    g.drawText (TRANS ("Loading plugins"), area.removeFromTop (26), juce::Justification::centredLeft, true);

    area.removeFromTop (6);
    auto progressRow = area.removeFromTop (20);
    g.setFont (juce::FontOptions (Theme::bodySize));
    const auto count = juce::String (done) + " / " + juce::String (total);
    const int countWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), "000 / 000") + 8;
    g.drawText (count, progressRow.removeFromRight (countWidth), juce::Justification::centredRight, false);
    progressRow.removeFromRight (8);
    auto bar = progressRow.withSizeKeepingCentre (progressRow.getWidth(), 8).toFloat();
    g.setColour (Theme::rackBackground);
    g.fillRoundedRectangle (bar, 4.0f);
    if (total > 0 && done > 0)
    {
        g.setColour (Theme::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jmin (done, total) / (float) total), 4.0f);
    }

    area.removeFromTop (10);
    g.setColour (Theme::subText);
    if (next.isNotEmpty())
        g.drawText (TRANS ("Loading: %name%").replace ("%name%", next), area.removeFromTop (20), juce::Justification::centredLeft, true);

    area.removeFromTop (8);
    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (Theme::smallSize));
    g.drawFittedText (TRANS ("The sound passes through without the plugins until all are loaded."), area, juce::Justification::topLeft, 2);
}

void MainComponent::saveSettings()
{
    if (auto xml = engine.saveState().createXml())
        settings.setValue ("rack", xml.get());

    settings.setValue ("endpointGuid", engine.getEndpointGuid());
    settings.setValue ("endpointName", engine.getEndpointName()); // 次に起動したときデバイスがなくても名前を出せるように
    settings.setValue ("presetModified", presetModified && ! baselineAfterLoad); // 次に起動したとき、プリセットから変更があったかを引き継ぐ（読み込み中は変更なし）

    // プラグインの選択画面で前回選んでいた行（次に開いたとき、その行を選んで同じ位置に出す）
    settings.setValue ("lastBrowserPlugin", PluginBrowser::getLastIdentifier());
    settings.setValue ("lastBrowserOffset", PluginBrowser::getLastOffset());
    settings.setValue ("browserSort", (int) PluginBrowser::getSortOrder());
    settings.setValue ("browserEffectsOnly", PluginBrowser::isEffectsOnly());

    // 自動のときは、自動で決まった量を次の起動の出発点にする
    if (bufferAutoToggle.getToggleState() && engine.isProcessingBufferOn())
        settings.setValue ("processingBufferMs", juce::roundToInt (engine.getProcessingBufferCurrentMs()));
    settings.saveIfNeeded();
    settingsDirty = false;
}
