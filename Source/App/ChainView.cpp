#include "ChainView.h"

#include "PluginActivityView.h"
#include "SectionMeter.h"
#include "Text.h"
#include "Theme.h"
#include "WheelSteps.h"

namespace
{
// 縦の並び（ChainView の座標）
constexpr int padX = 12;
constexpr int padTop = 6;
constexpr int padBottom = 10;
constexpr int sectionHeight = 53; // 入力・出力の欄（ラベルの行 + ゲインの行 + メーターの目盛り）
constexpr int arrowHeight = 8;        // プラグインの行の間の矢印
constexpr int rowHeight = 44;
constexpr int addGap = 6;             // 最後の行から「＋ プラグインを追加」まで
constexpr int addHeight = 22;
constexpr int outputArrowHeight = 10; // 「＋ プラグインを追加」から出力の欄まで
constexpr float arrowThickness = 1.3f;  // 矢印（と「＋ プラグインを追加」へつながる線）の太さ

// 入力・出力の欄の中の配置: 上に小さな文字のラベルの行（スライダーの上に「ゲイン」、メーターの上に種類の名前、右に最終的なレイテンシー）、
// その下にゲインの行（見出しの札、スライダー、メーターと右の数値）。メーターの目盛りの数値はゲインの行の下にはみ出す
constexpr int labelRowY = 5;
constexpr int labelRowHeight = 14;
constexpr int gainRowY = 20;
constexpr int gainRowHeight = 22;

void drawArrow (juce::Graphics& g, float x, float top, float bottom)
{
    juce::Path path;
    // 矢印の間が短い（8〜10）ので頭も小さくする: 先は下端の 1 上、頭の長さ 3・半分の幅 3
    constexpr float head = 3.0f;
    const float tip = bottom - 1.0f;
    path.startNewSubPath (x, top);
    path.lineTo (x, tip);
    path.startNewSubPath (x - head, tip - head);
    path.lineTo (x, tip);
    path.lineTo (x + head, tip - head);
    g.setColour (Theme::arrow);
    g.strokePath (path, juce::PathStrokeType (arrowThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// 小さな点（「⋯」のボタンと、行の左のつまみで同じ大きさ）
constexpr float dotSize = 2.4f, dotPitch = 4.0f;

void drawDot (juce::Graphics& g, float cx, float cy)
{
    g.fillEllipse (cx - dotSize * 0.5f, cy - dotSize * 0.5f, dotSize, dotSize);
}

// 「⋯」のボタン
class DotsButton final : public juce::Button
{
public:
    DotsButton() : Button (TRANS ("Menu")) {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto area = getLocalBounds().toFloat();
        if (highlighted || down)
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.18f : 0.10f));
            g.fillRoundedRectangle (area, 4.0f);
        }

        g.setColour (Theme::subText);
        const auto centre = area.getCentre();
        for (int i = -1; i <= 1; ++i)
            drawDot (g, centre.x + (float) i * dotPitch, centre.y);
    }
};

void drawCard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill, juce::Colour border)
{
    g.setColour (fill);
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (border);
    g.drawRoundedRectangle (area.reduced (0.75f), 8.0f, 1.5f);
}

} // namespace

//==============================================================================
// ゲインのスライダー。マウスホイールは 1 目盛りで 0.5 dB ずつ動かす（0.5 dB の倍数にそろえる）。
// JUCE の既定はスライダーの幅に対する割合で動くので、幅によって 1 目盛りの量が変わってしまう
class GainSlider final : public juce::Slider
{
public:
    static constexpr double wheelStepDb = 0.5;
    static constexpr int textBoxWidth = 68; // "-24.0 dB" が入る幅

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! isEnabled())
        {
            Slider::mouseWheelMove (e, wheel);
            return;
        }

        const int steps = wheelSteps.take (WheelSteps::verticalOrHorizontal (wheel));
        if (steps == 0)
            return;

        double value = getValue();
        for (int i = 0; i < std::abs (steps); ++i)
            value = steps > 0 ? std::floor (value / wheelStepDb + 1.0e-6) * wheelStepDb + wheelStepDb
                              : std::ceil (value / wheelStepDb - 1.0e-6) * wheelStepDb - wheelStepDb;

        setValue (juce::jlimit (getMinimum(), getMaximum(), value), juce::sendNotificationSync);
    }

private:
    WheelSteps wheelSteps;
};

// 入力・出力の欄（ゲインとメーター。出力の欄には最終的なレイテンシーも出す）。
// メーターの種類（SectionMeter::Kind）は、メーターの上のホイールか右クリックのメニューで切り替える
class ChainView::GainSection final : public juce::Component
{
public:
    GainSection (RackModel& modelToUse, bool isInputSection)
        : model (modelToUse), isInput (isInputSection)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, GainSlider::textBoxWidth, gainRowHeight);
        slider.setRange (-24.0, 24.0, 0.1);
        slider.setTextValueSuffix (" dB");
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setColour (juce::Slider::thumbColourId, Theme::accent);
        slider.setColour (juce::Slider::trackColourId, Theme::accent.withAlpha (0.7f));
        slider.setTitle (isInput ? TRANS ("Input gain") : TRANS ("Output gain"));
        slider.onValueChange = [this]
        {
            if (isInput)
                model.setInputGainDb ((float) slider.getValue());
            else
                model.setOutputGainDb ((float) slider.getValue());
        };
        addAndMakeVisible (slider);

        // 音声テストのチェックボックスは、この欄ではなく画面の上の欄に置く（TestToneToggles）

        refresh();
    }

    void refresh()
    {
        slider.setValue (isInput ? model.getInputGainDb() : model.getOutputGainDb(), juce::dontSendNotification);
        subtitle = isInput ? model.getInputSubtitle() : model.getOutputSubtitle();
        isMerge = ! isInput && model.outputIsMerge();
        refreshLatency();
        repaint();
    }


    void refreshLatency()
    {
        if (isInput)
            return;

        const auto text = model.describeTotalLatency();
        if (text != latencyText)
        {
            latencyText = text;
            repaint();
        }
    }

    void setMeter (const LevelReading& reading)
    {
        meter.update (reading, kind());

        // 種類がほかの欄・ほかの画面で変わっていたら、見出しの行の種類の名前も描き直す
        if (kind() != shownKind)
        {
            shownKind = kind();
            repaint();
            return;
        }

        const int x = 84 + sliderWidth();
        repaint (x, gainRowY, getWidth() - x, 1 + meterHeight + 16); // メーターと目盛り、右の数値
    }

    // ピークホールドを出すか（すべての入力・出力の欄に共通。表示メニューで切り替える。設定の peakHold。既定は ON）
    static void setPeakHoldEnabled (bool enabled) { peakHoldEnabled = enabled; }
    static bool isPeakHoldEnabled() { return peakHoldEnabled; }

    // メーターの種類（入力・出力の欄のすべてで共通。どこで変えてもほかの欄も同じ種類になる。設定の sectionMeter）
    static SectionMeter::Kind getKind() { return sharedKind; }
    static void setKind (SectionMeter::Kind newKind) { sharedKind = newKind; }
    inline static std::function<void()> onKindChanged; // 種類かピークホールドをホイールや右クリックのメニューで変えたとき（設定に保存する）

    // メーターの上のホイールで種類を切り替える（それ以外の場所ではラックのスクロール）
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! isOverMeter (e.getPosition()))
        {
            Component::mouseWheelMove (e, wheel);
            return;
        }

        const int steps = wheelSteps.take (WheelSteps::vertical (wheel));
        if (steps == 0)
            return;
        chooseKind (SectionMeter::next (kind(), -steps)); // 下へ回すと次へ
    }

    // メーターの上の右クリックで種類の一覧。インテグレーテッドのラウドネスのときは、左クリックで入力と出力の両方の値をリセットする
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isOverMeter (e.getPosition()))
            return;

        if (! e.mods.isPopupMenu())
        {
            if (kind() == SectionMeter::Kind::lufsIntegrated && e.mods.isLeftButtonDown() && onResetIntegrated != nullptr)
                onResetIntegrated();
            return;
        }

        juce::PopupMenu menu;
        menu.addSectionHeader (TRANS ("Meter type"));
        const auto current = kind();
        const auto add = [&] (SectionMeter::Kind k)
        {
            menu.addItem (SectionMeter::name (k), true, k == current, [safeThis = juce::Component::SafePointer<GainSection> (this), k]
            {
                if (safeThis != nullptr)
                    safeThis->chooseKind (k);
            });
        };
        for (const auto k : SectionMeter::order())
        {
            if (SectionMeter::startsGroup (k)) // まとまりごとに区切る
                menu.addSeparator();
            add (k);
        }
        if (current == SectionMeter::Kind::lufsIntegrated)
        {
            menu.addSeparator();
            menu.addItem (TRANS ("Reset integrated loudness"), [safeThis = juce::Component::SafePointer<GainSection> (this)]
            {
                if (safeThis != nullptr && safeThis->onResetIntegrated != nullptr)
                    safeThis->onResetIntegrated();
            });
        }

        // ピークホールド（表示メニューの「入出力のメーター」と同じ。すべての入力・出力の欄に共通）
        menu.addSeparator();
        menu.addItem (TRANS ("Peak hold"), true, peakHoldEnabled, []
        {
            peakHoldEnabled = ! peakHoldEnabled;
            if (onKindChanged != nullptr)
                onKindChanged(); // 設定に保存する
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // インテグレーテッドのラウドネスをリセットする（入力と出力の欄はいっしょにリセットする。ChainView が onResetIntegrated で両方を呼ぶ）
    std::function<void()> onResetIntegrated;

    void resetIntegrated()
    {
        meter.resetIntegrated();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        drawCard (g, getLocalBounds().toFloat(), Theme::card, Theme::cardBorder);

        // 見出しの札（入力 / 出力 / 合流）: ゲインの行の左端（スライダーの手前）
        const juce::Rectangle<float> pill (16.0f, (float) gainRowY + 1.0f, 62.0f, (float) gainRowHeight - 2.0f);
        g.setColour (isInput ? Theme::inputPill : (isMerge ? Theme::mergePill : Theme::outputPill));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (Theme::bodySize, juce::Font::bold));
        g.drawText (isInput ? TRANS ("Input") : (isMerge ? TRANS ("Merge") : TRANS ("Output")), pill, juce::Justification::centred);

        // ラベルの行（小さな文字）: スライダーの上に「ゲイン」とその右に説明（アプリ用ラックだけ）、メーターの真上に種類の名前、
        // 右端に最終的なレイテンシー（出力の欄だけ）
        auto labelRow = juce::Rectangle<int> (gainLabelX, labelRowY, getWidth() - 16 - gainLabelX, labelRowHeight);
        if (latencyText.isNotEmpty())
        {
            g.setFont (latencyFont());
            const int width = juce::jmin (labelRow.getWidth() / 2 + 120, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), latencyText) + 8);
            g.setColour (Theme::accent);
            g.drawText (latencyText, labelRow.removeFromRight (width), juce::Justification::centredRight, true);
        }
        if (const auto label = kindLabelArea(); ! label.isEmpty())
        {
            g.setColour (Theme::subText);
            g.setFont (juce::FontOptions (Theme::smallSize));
            g.drawText (SectionMeter::name (kind()), label, juce::Justification::centredLeft, true);
            labelRow.setRight (juce::jmin (labelRow.getRight(), label.getX() - 12));
        }
        g.setColour (Theme::subText);
        g.setFont (juce::FontOptions (Theme::smallSize));
        const auto gainText = TRANS ("Gain");
        g.drawText (gainText, labelRow, juce::Justification::centredLeft, true);
        if (subtitle.isNotEmpty())
        {
            g.setColour (Theme::dimText);
            g.drawText (subtitle, labelRow.withTrimmedLeft (juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::smallSize), gainText) + 12),
                        juce::Justification::centredLeft, true);
        }

        // ゲインのスライダーの目盛り（-24 / -12 / 0 / +12 / +24 dB。つまみの下に短い線と数値。数値の高さと書式はメーターの目盛りにそろえる）
        {
            g.setFont (juce::FontOptions (10.0f));
            const float tickTop = (float) (gainRowY + 16), labelTop = (float) (gainRowY + 1 + meterHeight + 4);
            for (const int db : { -24, -12, 0, 12, 24 })
            {
                const float x = (float) slider.getX() + (float) slider.getPositionOfValue ((double) db);
                g.setColour (Theme::dimText);
                g.fillRect (x - 0.5f, tickTop, 1.0f, labelTop - tickTop - 1.0f);
                const auto label = (db > 0 ? "+" : "") + juce::String (db);
                const float labelWidth = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label) + 2.0f;
                g.drawText (label, juce::Rectangle<float> (x - labelWidth * 0.5f, labelTop, labelWidth, 11.0f), juce::Justification::centred, false);
            }
        }

        // メーター（枠の中に左（上）と右（下）の 2 本のバー。幅が足りないときは出さない）と、その下の目盛り。右に数値（左右の大きい方）
        meter.paint (g, kind(), meterArea().toFloat(), readoutArea(), peakHoldEnabled);
    }

    // スライダーのつまみが動く部分とメーターを同じくらいの幅に分ける（つまみの部分は minTrackWidth〜maxTrackWidth）
    void resized() override
    {
        slider.setBounds (84, gainRowY, sliderWidth(), gainRowHeight);
    }

private:
    // メーターの枠の高さ（左右 2 本のバーが入る。1 本だった頃の 9 の約 1.5 倍）
    static constexpr int meterHeight = 14;

    SectionMeter::Kind kind() const { return getKind(); }

    void chooseKind (SectionMeter::Kind newKind)
    {
        if (newKind == kind())
            return;
        setKind (newKind);
        shownKind = newKind;
        repaint(); // ほかの欄は、次のメーターの更新で描き直す（setMeter）
        if (onKindChanged != nullptr)
            onKindChanged();
    }

    // 右の数値の欄（メーターの右）
    juce::Rectangle<int> readoutArea() const { return { getWidth() - 88, gainRowY, 72, gainRowHeight }; }

    // メーターの種類の名前の欄（見出しの行の、メーターの真上。右は最終的なレイテンシーか音声テストのボタンの手前まで）。メーターがなければ空
    juce::Rectangle<int> kindLabelArea() const
    {
        const auto area = meterArea();
        if (area.isEmpty() || kind() == SectionMeter::Kind::none) // 表示しないときは名前も出さない
            return {};
        const int width = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::smallSize), SectionMeter::name (kind())) + 4;
        int right = getWidth() - 16;
        if (latencyText.isNotEmpty())
            right -= juce::GlyphArrangement::getStringWidthInt (latencyFont(), latencyText) + 16;
        return { area.getX(), labelRowY, juce::jmax (0, juce::jmin (width, right - area.getX())), labelRowHeight };
    }

    static juce::FontOptions latencyFont() { return juce::FontOptions (Theme::smallSize, juce::Font::bold); } // 最終的なレイテンシー（ラベルの行）
    static constexpr int gainLabelX = 88; // 「ゲイン」の左端（スライダーのつまみが動く範囲の左端の少し内側）

    // ホイールと右クリックを受ける範囲: メーターと目盛り、右の数値、見出しの行の種類の名前
    bool isOverMeter (juce::Point<int> position) const
    {
        const auto area = meterArea();
        const int left = area.isEmpty() ? readoutArea().getX() : area.getX();
        return juce::Rectangle<int> (left, gainRowY - 2, getWidth() - 16 - left, meterHeight + 20).contains (position)
            || kindLabelArea().contains (position);
    }

    static constexpr int minTrackWidth = 110;
    static constexpr int maxTrackWidth = 300;
    static constexpr int minMeterWidth = 40;

    // スライダー（数値の欄を含む）とメーターに使える幅（間のすき間と、右の数値（-inf dB）の欄は除く）
    int availableWidth() const { return getWidth() - 84 - 16 - 92; }

    // スライダーの幅（右の数値の欄を含む）。つまみの部分は、数値の欄を除いた残りの半分（残りの半分はメーター）
    int sliderWidth() const
    {
        const int track = juce::jlimit (minTrackWidth, maxTrackWidth, (availableWidth() - GainSlider::textBoxWidth) / 2);
        return juce::jmin (juce::jmax (0, availableWidth()), GainSlider::textBoxWidth + track);
    }

    juce::Rectangle<int> meterArea() const
    {
        const int x = 84 + sliderWidth() + 16;
        const int width = getWidth() - x - 92;
        if (width < minMeterWidth)
            return {};
        return { x, gainRowY + 1, width, meterHeight }; // 下に目盛りの数値を出すので上に寄せる
    }

    RackModel& model;
    const bool isInput;
    bool isMerge = false; // 最後の欄が「合流」（アプリ用ラック）
    GainSlider slider;
    juce::String subtitle, latencyText;
    SectionMeter meter;
    WheelSteps wheelSteps;
    inline static bool peakHoldEnabled = true;
    inline static SectionMeter::Kind sharedKind = SectionMeter::Kind::peakRms; // 既定はピーク＋RMS
    SectionMeter::Kind shownKind = sharedKind; // この欄が最後に描いた種類（ほかの欄で変わったのに気づくため）
};

//==============================================================================
// プラグイン 1 つ分の行
class ChainView::PluginRow final : public juce::Component
{
public:
    PluginRow (ChainView& ownerView, int id)
        : owner (ownerView), pluginId (id)
    {
        onOffButton.setClickingTogglesState (true);
        onOffButton.onClick = [this] { owner.getChain().setEnabled (pluginId, onOffButton.getToggleState()); };
        addAndMakeVisible (onOffButton);

        // プラグインの画面は行のダブルクリック（または ⋯ メニュー、メーターかエディターの画面の縮小のクリック）で開く

        menuButton.onClick = [this] { showMenu (false); };
        addAndMakeVisible (menuButton);

        // 名前の右の、増減のメーターと、その右のエディターの画面の縮小。マウスは行が受ける
        addChildComponent (activity);
        addChildComponent (snapshotView);
    }

    int getPluginId() const { return pluginId; }

    // メーターとエディターの画面を進める（タイマーから 1 秒に 20 回）
    void updateActivity()
    {
        const auto* plugin = owner.getChain().findPlugin (pluginId);
        if (activity.isVisible())
            activity.update (plugin);
        if (snapshotView.isVisible())
            snapshotView.update (plugin);
    }

    void refresh (int index, bool chainBypassed)
    {
        number = index + 1;
        numberDigits = juce::String ((int) owner.getChain().getPlugins().size()).length();

        problem = {};

        if (const auto* plugin = owner.getChain().findPlugin (pluginId))
        {
            name = plugin->getDisplayName();
            maker = plugin->description.manufacturerName;
            enabled = plugin->enabled.load();
            loaded = plugin->instance != nullptr;

            // 不具合があれば、メーカー名の代わりに理由と直し方を出す
            if (! loaded && plugin->pendingLoad)
                problem = TRANS ("Loading...");
            else if (! loaded && plugin->deferredLoad && plugin->loadError.isEmpty())
                problem = TRANS ("Not loaded (safe mode). Turn it ON to load it");
            else if (! loaded)
                problem = TRANS ("Could not be loaded (use ... > \"Reload plugin\" to try again)")
                        + (plugin->loadError.isNotEmpty() ? ": " + plugin->loadError : juce::String());
            else if (plugin->faulted.load())
                problem = TRANS ("Stopped after an error and is bypassed (use ... > \"Reload plugin\" to load it again)");
        }

        active = enabled && loaded && problem.isEmpty() && ! chainBypassed;

        onOffButton.setToggleState (enabled, juce::dontSendNotification);
        onOffButton.setTitle ((enabled ? TRANS ("Turn %name% off") : TRANS ("Turn %name% on")).replace ("%name%", name));
        onOffButton.setColour (juce::TextButton::buttonColourId, Theme::off);
        onOffButton.setColour (juce::TextButton::textColourOffId, Theme::subText);
        onOffButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        updateOnOffLook();

        refreshLatency();
        setAlpha (active || dragging ? 1.0f : 0.55f);
        resized(); // 行の表示を出すか（表示を変えたとき）
        updateActivity();
        repaint();
    }

    // プラグインのレイテンシー（途中で変わることがあるので、タイマーからも呼ぶ）
    void refreshLatency()
    {
        int samples = -1;
        if (const auto* plugin = owner.getChain().findPlugin (pluginId); plugin != nullptr && plugin->instance != nullptr)
            samples = PluginChain::getLatencySamples (*plugin);

        // 欄の幅は、ラックのプラグインのうち一番長いサンプル数の桁数に合わせる（どの行も同じ幅にして列をそろえる）
        int digits = 1;
        for (const auto& other : owner.getChain().getPlugins())
            if (other->instance != nullptr)
                digits = juce::jmax (digits, juce::String (PluginChain::getLatencySamples (*other)).length());

        if (samples == latencySamples && digits == latencyDigits)
            return;

        latencySamples = samples;
        if (digits != latencyDigits)
        {
            latencyDigits = digits;
            resized();
        }
        repaint();
    }

    // 休止中か（タイマーから呼ぶ）。再生デバイスで「有効」になっていなければ、ON のプラグインはすべて休止中。
    // そうでなければ無音のための休止で、全体の「無音時に休止」が OFF なら出さない
    void refreshResting()
    {
        bool now = false;
        const bool deviceOff = ! PluginChain::isDeviceEnabled();
        if (const auto* plugin = owner.getChain().findPlugin (pluginId); plugin != nullptr && plugin->enabled.load())
            now = deviceOff || (PluginChain::isRestWhenSilent() && plugin->resting.load() && ! plugin->alwaysProcess.load());

        if (now == resting && deviceOff == restingForDevice)
            return;

        resting = now;
        restingForDevice = deviceOff;
        updateOnOffLook();
    }

    // ON / OFF / Resting（ON だが休止中。日本語では「休止」）の表示
    void updateOnOffLook()
    {
        const bool showResting = enabled && resting;
        onOffButton.setButtonText (showResting ? TRANS ("Resting") : (enabled ? "ON" : "OFF"));
        onOffButton.setColour (juce::TextButton::buttonOnColourId, showResting ? Theme::resting : Theme::on);
        onOffButton.setColour (juce::ComboBox::outlineColourId, showResting ? Theme::resting.brighter (0.5f)
                                                                : (enabled ? Theme::accent : Theme::buttonBorder));
        onOffButton.setTooltip (! showResting   ? juce::String()
                                : restingForDevice ? TRANS ("Resting: PRI System Rack is not enabled on the playback device, so this plugin is not processing. Click \"Enabled\" next to the playback device to start.")
                                                   : TRANS ("Resting: the audio is silent, so this plugin is not processing. It resumes as soon as sound arrives."));
    }

    void setDragging (bool isDragging)
    {
        dragging = isDragging;
        setAlpha (active || dragging ? 1.0f : 0.55f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        drawCard (g, getLocalBounds().toFloat(), dragging ? Theme::cardRaised : Theme::card, dragging ? Theme::accent : Theme::cardBorder);

        // 左端のつまみ
        g.setColour (Theme::dimText);
        const float cy = (float) getHeight() * 0.5f;
        for (int column = 0; column < 2; ++column)
            for (int row = -1; row <= 1; ++row)
                drawDot (g, gripX + (float) column * dotPitch, cy + (float) row * dotPitch);

        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (Theme::bodySize));
        // 番号は ON/OFF ボタンのすぐ右（プラグインの数の桁数が入る幅）。名前はその右に詰める
        g.drawText (juce::String (number), juce::Rectangle<int> (numberX(), 0, numberWidth(), getHeight()), juce::Justification::centredLeft);

        // レイテンシー（⋯ ボタンの左）
        const auto latencyArea = juce::Rectangle<int> (latencyX(), 0, latencyWidth(), getHeight());
        if (latencySamples >= 0)
        {
            const double rate = (double) owner.getChain().getSampleRate();
            const double ms = rate > 0.0 ? 1000.0 * latencySamples / rate : 0.0;
            g.setColour (latencySamples > 0 ? Theme::text : Theme::dimText);
            g.setFont (juce::FontOptions (Theme::bodySize, juce::Font::bold));
            g.drawText (juce::String (ms, 1) + " ms", latencyArea.withTrimmedTop (5).withHeight (18), juce::Justification::centredRight);
            g.setColour (Theme::dimText);
            g.setFont (juce::FontOptions (Theme::smallSize - 1.0f));
            g.drawText (TRANS ("latency %samples% smp").replace ("%samples%", juce::String (latencySamples)),
                        latencyArea.withTrimmedTop (23).withHeight (15), juce::Justification::centredRight);
        }

        // 名前とメーカー（エディターの画面かメーターがあればその左まで）。名前は本文と同じ大きさの太字
        const int textRight = snapshotView.isVisible() ? snapshotView.getX() - 10
                            : activity.isVisible()     ? activity.getX() - 10
                                                       : latencyArea.getX() - 12;
        const auto textArea = juce::Rectangle<int> (textX(), 4, textRight - textX(), getHeight() - 8);
        g.setColour (active ? Theme::text : Theme::dimText);
        g.setFont (juce::FontOptions (Theme::bodySize, juce::Font::bold));
        g.drawText (name, textArea.withHeight (19), juce::Justification::centredLeft, true);
        g.setColour (problem.isNotEmpty() ? Theme::error : Theme::subText);
        g.setFont (juce::FontOptions (Theme::smallSize));
        g.drawText (problem.isNotEmpty() ? problem : maker, textArea.withTrimmedTop (19).withHeight (16),
                    juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        const int buttonHeight = Theme::controlHeight;
        const int y = (getHeight() - buttonHeight) / 2;
        onOffButton.setBounds (onOffX, y, onOffWidth(), buttonHeight);
        menuButton.setBounds (getWidth() - menuButtonRight - menuButtonWidth, (getHeight() - menuButtonHeight) / 2, menuButtonWidth, menuButtonHeight);

        // レイテンシーの左に、右からメーターとエディターの画面の縮小。名前の幅（minNameWidth）を残して入るだけの幅を使う。
        // 足りなければメーターを先に詰め、それでも足りなければメーター、エディターの画面の順に出さない。
        // メーターが「表示しない」のときも場所は残す（何も描かないが、ホイールで別の表示に戻せるように）
        const int right = latencyX() - gap;
        const int available = right - textX() - minNameWidth - 10;
        int snapshotWidth = 0, meterWidth = 0;
        if (available >= minActivityWidth + gap + minSnapshotWidth)
        {
            snapshotWidth = juce::jlimit (minSnapshotWidth, maxSnapshotWidth, available - gap - minActivityWidth);
            meterWidth = juce::jmin (maxActivityWidth, available - gap - snapshotWidth);
        }
        else if (available >= minSnapshotWidth)
        {
            snapshotWidth = juce::jmin (maxSnapshotWidth, available);
        }

        // 並びは左から、エディターの画面の縮小、メーター（メーターがレイテンシーの隣）
        const int top = 5, height = getHeight() - 10;
        const int meterX = right - meterWidth;
        activity.setVisible (meterWidth > 0);
        activity.setBounds (meterX, top, meterWidth, height);
        snapshotView.setVisible (snapshotWidth > 0);
        snapshotView.setBounds (meterX - (meterWidth > 0 ? gap : 0) - snapshotWidth, top, snapshotWidth, height);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto position = e.getPosition();
        // 指のカーソルは、クリックで画面が開くエディターの縮小画面だけ（メーターはクリックしても何も起きないので普通の矢印）
        setMouseCursor (position.x < gripRight               ? juce::MouseCursor::DraggingHandCursor
                        : isOver (snapshotView, position)    ? juce::MouseCursor::PointingHandCursor
                                                             : juce::MouseCursor::NormalCursor);
    }

    // メーターの上のホイールで表示を切り替える（PluginActivityView::viewOrder の順。最後が「表示しない」）。
    // それ以外の場所ではラックのスクロール
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! isOver (activity, e.getPosition()))
        {
            Component::mouseWheelMove (e, wheel);
            return;
        }

        const int steps = wheelSteps.take (WheelSteps::vertical (wheel));
        if (steps == 0)
            return;

        if (const auto* plugin = owner.getChain().findPlugin (pluginId))
            owner.getChain().setView (pluginId, PluginActivityView::nextView (plugin->view, -steps)); // 下へ回すと次へ
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            // メーターの上なら、表示の切り替えだけを出す
            if (isOver (activity, e.getPosition()))
                showViewMenu();
            else
                showMenu (true);
            return;
        }

        dragStarted = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! e.mods.isLeftButtonDown())
            return;

        // 少し動かしてから並べ替えを始める（クリックやダブルクリックと区別する）
        if (! dragStarted)
        {
            if (e.getDistanceFromDragStart() < 4)
                return;

            dragStarted = true;
            owner.rowDragStarted (*this, e);
        }

        owner.rowDragged (e.getEventRelativeTo (&owner));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragStarted)
            owner.rowDragEnded();
        else if (e.mods.isLeftButtonDown() && e.mouseWasClicked() && loaded && isOver (snapshotView, e.getMouseDownPosition()))
            owner.getCallbacks().openEditor (pluginId); // エディターの画面のクリックでエディターを開く（メーターはダブルクリックだけ）

        dragStarted = false;
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        owner.getCallbacks().openEditor (pluginId);
    }

private:
    // 左から: つまみ、ON/OFF、番号、名前
    // 左端のつまみ（ドラッグで並べ替えられる印。点 2 列 × 3 段。点は ⋯ ボタンと同じ大きさ）。並べ替えは行のどこをドラッグしてもできる
    static constexpr float gripX = 12.0f;                             // つまみの左の列の点の中心
    static constexpr int gripRight = 12 + 4 + 6;                       // つまみの右端（ここまでをつまみの範囲とし、カーソルを変える）
    static constexpr int onOffX = gripRight + 4;                       // ON/OFF ボタンの左端

    // ON/OFF ボタンの幅: ON / OFF / Resting（日本語では「休止」）の長い方の文字 + 左右 8 px
    static int onOffWidth()
    {
        const juce::FontOptions font (Theme::buttonTextSize);
        int width = 0;
        for (const auto& text : { juce::String ("ON"), juce::String ("OFF"), TRANS ("Resting") })
            width = juce::jmax (width, juce::GlyphArrangement::getStringWidthInt (font, text));
        return width + 16;
    }

    static int numberX() { return onOffX + onOffWidth() + 8; }

    // 番号の欄の幅（プラグインの数の桁数ぶん。どの行も同じ幅にして名前の位置をそろえる）
    int numberWidth() const
    {
        return juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::bodySize), juce::String::repeatedString ("0", numberDigits)) + 1;
    }

    int textX() const { return numberX() + numberWidth() + 4; }

    // レイテンシーの欄の幅: 「000.0 ms」と「レイテンシー 0000 smp」（桁数はラックで一番長いサンプル数）の長い方
    int latencyWidth() const
    {
        const int ms = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::bodySize, juce::Font::bold), "000.0 ms");
        const int samples = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::smallSize - 1.0f),
                                                                       TRANS ("latency %samples% smp").replace ("%samples%", juce::String::repeatedString ("0", latencyDigits)));
        return juce::jmax (ms, samples) + 2;
    }
    // メーターとエディターの画面の幅（名前に minNameWidth を残して、入るだけ）
    static constexpr int minNameWidth = 100;
    static constexpr int minActivityWidth = 48;
    static constexpr int maxActivityWidth = 130;
    static constexpr int minSnapshotWidth = 40;
    static constexpr int maxSnapshotWidth = 64;
    static constexpr int gap = 6;

    int latencyX() const { return getWidth() - menuButtonRight - menuButtonWidth - menuButtonGap - latencyWidth(); } // レイテンシーの欄（⋯ ボタンの左）の左端
    static constexpr int menuButtonWidth = 18, menuButtonHeight = 18; // ⋯ ボタン
    static constexpr int menuButtonRight = 7; // ⋯ ボタンの右（行の右端まで）
    static constexpr int menuButtonGap = 3;   // レイテンシーの欄と ⋯ ボタンの間

    static bool isOver (const juce::Component& child, juce::Point<int> position)
    {
        return child.isVisible() && child.getBounds().contains (position);
    }

    // メーターの表示の選択肢（⋯ メニューの「行の表示」と、メーターの右クリック）
    juce::PopupMenu createViewMenu()
    {
        using View = PluginChain::Plugin::View;
        const auto* plugin = owner.getChain().findPlugin (pluginId);
        const auto current = plugin != nullptr ? plugin->view : View::none;

        juce::PopupMenu menu;
        auto add = [&] (View view)
        {
            menu.addItem (PluginActivityView::viewName (view), true, view == current,
                          [safeOwner = juce::Component::SafePointer<ChainView> (&owner), id = pluginId, view]
                          {
                              if (safeOwner != nullptr)
                                  safeOwner->getChain().setView (id, view);
                          });
        };
        for (const auto view : PluginActivityView::viewOrder())
        {
            if (view == View::none)
                menu.addSeparator();
            add (view);
        }
        return menu;
    }

    void showViewMenu()
    {
        createViewMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // atMouse: 右クリックならマウスの位置に、⋯ ボタンならボタンの位置に出す
    void showMenu (bool atMouse)
    {
        // 更新・置き換え・削除はこの行を作り直す（または消す）ので、メニューの処理が終わってから行う。
        // 項目は JUCE があとで（callAsync で）呼ぶので、この行（this）ではなく ChainView と ID を持たせる
        auto later = [safeOwner = juce::Component::SafePointer<ChainView> (&owner), id = pluginId] (auto member)
        {
            return [safeOwner, id, member]
            {
                juce::MessageManager::callAsync ([safeOwner, id, member]
                {
                    if (safeOwner != nullptr)
                        if (const auto& callback = safeOwner->getCallbacks().*member)
                            callback (id);
                });
            };
        };

        juce::PopupMenu menu;
        menu.addItem (TRANS ("Open editor"), loaded, false, later (&Callbacks::openEditor));
        menu.addItem (TRANS ("Duplicate"), loaded && ! owner.getChain().isFull(), false, later (&Callbacks::duplicatePlugin));
        menu.addSeparator();
        menu.addItem (TRANS ("Reload plugin"), later (&Callbacks::reloadPlugin));
        menu.addItem (TRANS ("Replace with another plugin..."), later (&Callbacks::replacePlugin));
        menu.addSeparator();

        // 無音時の休止をこのプラグインだけしない（ノイズを足すものなど）。全体の「無音時に休止」が ON のときに効く
        const auto* plugin = owner.getChain().findPlugin (pluginId);
        const bool alwaysProcess = plugin != nullptr && plugin->alwaysProcess.load();
        menu.addItem (TRANS ("Always process (do not rest when silent)"), true, alwaysProcess,
                      [safeOwner = juce::Component::SafePointer<ChainView> (&owner), id = pluginId, alwaysProcess]
                      {
                          if (safeOwner != nullptr)
                              safeOwner->getChain().setAlwaysProcess (id, ! alwaysProcess);
                      });
        menu.addSubMenu (TRANS ("Meter"), createViewMenu());
        menu.addSeparator();
        menu.addItem (TRANS ("Remove"), later (&Callbacks::removePlugin));

        auto options = juce::PopupMenu::Options();
        // 元の部品を必ず指定する（JUCE がその部品の倍率に合わせてメニューを拡大する）。右クリックのときは場所だけマウスの位置にする
        menu.showMenuAsync (atMouse ? options.withTargetComponent (this).withMousePosition() : options.withTargetComponent (&menuButton));
    }

    ChainView& owner;
    const int pluginId;
    int number = 0;
    juce::String name, maker, problem;
    int latencySamples = -2; // -1 = 読み込めていない
    int latencyDigits = 4;   // レイテンシーの欄の幅に使うサンプル数の桁数（ラックで一番長いもの）
    bool enabled = true;
    bool loaded = true;
    bool active = true;
    bool dragging = false;
    bool dragStarted = false;
    bool resting = false;
    bool restingForDevice = false; // 休止の理由が「再生デバイスで有効になっていない」（ツールチップを変える）
    int numberDigits = 1;          // プラグインの数の桁数（番号の欄の幅）

    juce::TextButton onOffButton;
    DotsButton menuButton;
    PluginActivityView activity;
    PluginSnapshotView snapshotView;
    WheelSteps wheelSteps; // メーターの上のホイール

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginRow)
};

//==============================================================================
ChainView::ChainView (RackModel& modelToUse, Callbacks callbacksToUse)
    : model (modelToUse), chain (modelToUse.getChain()), callbacks (std::move (callbacksToUse))
{
    inputSection = std::make_unique<GainSection> (model, true);
    outputSection = std::make_unique<GainSection> (model, false);
    addAndMakeVisible (*inputSection);
    addAndMakeVisible (*outputSection);

    // インテグレーテッドのラウドネスは、入力と出力のどちらでリセットしても両方をリセットする（同じ区間で比べられるように）
    for (auto* section : { inputSection.get(), outputSection.get() })
        section->onResetIntegrated = [this]
        {
            inputSection->resetIntegrated();
            outputSection->resetIntegrated();
        };

    addButton.setButtonText (TRANS ("+ Add plugin"));
    addButton.setColour (juce::TextButton::buttonColourId, Theme::rackBackground);
    addButton.setColour (juce::TextButton::textColourOffId, Theme::accent);
    addButton.setColour (juce::ComboBox::outlineColourId, Theme::accent);
    addButton.onClick = [this] { callbacks.addPlugin(); };
    addAndMakeVisible (addButton);

    // ラック（RackEngine / AppRack）がチェーンの変更もそのまま知らせてくるので、ラックの知らせだけを聞く（チェーンも聞くと作り直しが 2 回になる）
    model.getModelBroadcaster().addChangeListener (this);
    syncRows();

    startTimer (1000 / PluginActivityView::getFramesPerSecond()); // メーター
}

ChainView::~ChainView()
{
    stopTimer();
    model.getModelBroadcaster().removeChangeListener (this);
}

void ChainView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncRows();
}

void ChainView::syncRows()
{
    // 並べ替えのドラッグ中に並びが変わったら、ドラッグはやめる
    if (draggingRow != nullptr)
    {
        draggingRow->setDragging (false);
        draggingRow = nullptr;
        dropIndex = -1;
    }

    // チェーンの並びに合わせて行を作り直す（ある行は使い回す）
    std::vector<std::unique_ptr<PluginRow>> ordered;
    for (const auto& plugin : chain.getPlugins())
    {
        auto it = std::find_if (rows.begin(), rows.end(), [&plugin] (const auto& row) { return row != nullptr && row->getPluginId() == plugin->id; });

        if (it != rows.end())
        {
            ordered.push_back (std::move (*it));
        }
        else
        {
            ordered.push_back (std::make_unique<PluginRow> (*this, plugin->id));
            addAndMakeVisible (*ordered.back());
        }
    }
    rows = std::move (ordered);

    for (size_t i = 0; i < rows.size(); ++i)
        rows[i]->refresh ((int) i, chain.isBypassAll());

    mergedRacks = model.getMergedRacks();

    // プラグインの数が上限（PluginChain::maxPlugins）なら「＋ プラグインを追加」を押せなくする
    addButton.setEnabled (! chain.isFull());
    addButton.setTooltip (chain.isFull() ? PluginChain::describeFull() : juce::String());
    inputSection->refresh();
    outputSection->refresh();
    updateHeight();
    resized();
    repaint();
}

void ChainView::setPeakHoldEnabled (bool enabled)
{
    GainSection::setPeakHoldEnabled (enabled);
}

bool ChainView::isPeakHoldEnabled()
{
    return GainSection::isPeakHoldEnabled();
}

void ChainView::setSectionMeterKind (const juce::String& id)
{
    GainSection::setKind (SectionMeter::fromId (id));
}

juce::String ChainView::getSectionMeterKind()
{
    return SectionMeter::toId (GainSection::getKind());
}

void ChainView::setSectionMeterChangedCallback (std::function<void()> callback)
{
    GainSection::onKindChanged = std::move (callback);
}

void ChainView::setMinimumHeight (int height)
{
    minimumHeight = height;
    updateHeight();
}

void ChainView::updateHeight()
{
    const int height = juce::jmax (minimumHeight, contentHeight());
    if (height != getHeight())
        setSize (getWidth(), height);
}

void ChainView::updateMeters()
{
    inputSection->setMeter (model.takeInputLevels());
    outputSection->setMeter (model.takeOutputLevels());

    for (auto& row : rows)
        row->updateActivity();

    // 休止中の表示は 0.25 秒ごとに見直す。
    // レイテンシーはプラグインが途中で変えることがあるので、0.5 秒ごとに読み直す（メーターの fps によらない）
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastRestingCheckMs >= 250)
    {
        lastRestingCheckMs = now;
        for (auto& row : rows)
            row->refreshResting();
    }

    if (now - lastLatencyCheckMs >= 500)
    {
        lastLatencyCheckMs = now;
        for (auto& row : rows)
            row->refreshLatency();
        outputSection->refreshLatency();
    }
}

void ChainView::timerCallback()
{
    // 表示メニューで fps が変わったら、次の回から合わせる
    if (const int interval = 1000 / PluginActivityView::getFramesPerSecond(); getTimerInterval() != interval)
        startTimer (interval);

    updateMeters();
}

void ChainView::setMeterFramesPerSecond (int fps)
{
    PluginActivityView::setFramesPerSecond (fps);
}

int ChainView::getMeterFramesPerSecond()
{
    return PluginActivityView::getFramesPerSecond();
}

//==============================================================================
int ChainView::inputTop() const
{
    return padTop;
}

int ChainView::addTop() const
{
    return inputTop() + sectionHeight + (int) rows.size() * (arrowHeight + rowHeight) + addGap;
}

int ChainView::outputTop() const
{
    return addTop() + addHeight + outputArrowHeight;
}

int ChainView::outputHeight() const
{
    return sectionHeight;
}

int ChainView::slotTop (int index) const
{
    return inputTop() + sectionHeight + arrowHeight + index * (rowHeight + arrowHeight);
}

juce::Rectangle<int> ChainView::rowBounds (int index) const
{
    return { padX, slotTop (index), getWidth() - padX * 2, rowHeight };
}

int ChainView::contentHeight() const
{
    return outputTop() + outputHeight() + padBottom;
}

int ChainView::dropLineY (int insertIndex) const
{
    if (rows.empty())
        return addButton.getY() - addGap / 2;

    if (insertIndex < (int) rows.size())
        return slotTop (insertIndex) - arrowHeight / 2;

    return slotTop ((int) rows.size() - 1) + rowHeight + addGap / 2;
}

int ChainView::insertIndexAt (int centreY) const
{
    for (int i = 0; i < (int) rows.size(); ++i)
        if (centreY < slotTop (i) + rowHeight / 2)
            return i;

    return (int) rows.size();
}

void ChainView::resized()
{
    // 中身の高さに合わせる（setSize からもう一度ここに来る）
    if (const int height = juce::jmax (minimumHeight, contentHeight()); height != getHeight())
    {
        setSize (getWidth(), height);
        return;
    }

    const int width = getWidth() - padX * 2;
    inputSection->setBounds (padX, inputTop(), width, sectionHeight);

    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].get() != draggingRow)
            rows[i]->setBounds (rowBounds ((int) i));

    const int addWidth = Theme::buttonWidth (addButton, 18);
    addButton.setBounds ((getWidth() - addWidth) / 2, addTop(), addWidth, addHeight);
    outputSection->setBounds (padX, outputTop(), width, outputHeight());
}

void ChainView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::rackBackground);

    const float centreX = (float) getWidth() * 0.5f;

    // 流れの矢印
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const int top = slotTop ((int) i);
        drawArrow (g, centreX, (float) (top - arrowHeight), (float) top);
    }
    // 最後の行（行がなければ入力の欄）から「＋ プラグインを追加」までの線（矢印と同じ太さ・色。下の矢印へ流れがつながって見えるように）
    {
        const int fromY = rows.empty() ? inputSection->getBottom() : rowBounds ((int) rows.size() - 1).getBottom();
        if (addButton.getY() > fromY)
        {
            g.setColour (Theme::arrow);
            g.drawLine (centreX, (float) fromY, centreX, (float) addButton.getY(), arrowThickness);
        }
    }
    drawArrow (g, centreX, (float) addButton.getBottom(), (float) outputSection->getY());

    // 出力の手前で合流するアプリ用ラック（メインのラックだけ）: 「＋ プラグインを追加」の横に名前の札を並べ、
    // それぞれの札から出力の欄へ矢印でつなぐ。まずボタンの左（出力の欄の見出しの位置から）に並べ、入りきらなくなったら
    // ボタンの右に左詰めで続ける。右にも入りきらない分は出さない。
    // バイパス中のラックは、上の欄の札と同じく枠をバイパスの色に、名前を暗くする
    if (! mergedRacks.empty())
    {
        const juce::FontOptions font (Theme::smallSize, juce::Font::bold);
        const auto pillArea = addButton.getBounds().toFloat().reduced (0.0f, 1.0f);
        const float regions[2][2] { { (float) padX + 16.0f, (float) addButton.getX() - 12.0f },                // ボタンの左
                                    { (float) addButton.getRight() + 12.0f, (float) (getWidth() - padX - 16) } }; // ボタンの右
        int region = 0;
        float x = regions[0][0];
        g.setFont (font);
        for (const auto& rack : mergedRacks)
        {
            const float width = juce::jmax (40.0f, (float) juce::GlyphArrangement::getStringWidthInt (font, rack.name) + 24.0f);
            if (x + width > regions[region][1])
            {
                if (region == 1)
                    break;
                region = 1;
                x = regions[1][0];
                if (x + width > regions[1][1])
                    break;
            }

            const auto pill = pillArea.withX (x).withWidth (width);
            g.setColour (Theme::mergeFill);
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            g.setColour (rack.bypassed ? Theme::bypassOn : Theme::mergeBorder);
            g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, rack.bypassed ? 1.5f : 1.0f);
            g.setColour (rack.bypassed ? Theme::subText : Theme::mergeText);
            g.drawText (rack.name, pill.reduced (8.0f, 0.0f), juce::Justification::centred, true);

            drawArrow (g, pill.getCentreX(), pill.getBottom(), (float) outputSection->getY());
            x += width + 8.0f;
        }
    }

    if (draggingRow != nullptr)
    {
        // 元の位置（点線の枠）
        const auto slot = rowBounds (dragFromIndex).toFloat().reduced (0.5f);
        juce::Path outline;
        outline.addRoundedRectangle (slot, 8.0f);
        juce::Path dashed;
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (Theme::buttonBorder);
        g.fillPath (dashed);

        // つかんでいる行の影
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 24, { 0, 10 }).drawForRectangle (g, draggingRow->getBounds());
    }
}

void ChainView::paintOverChildren (juce::Graphics& g)
{
    // ファイルのドロップ: 全体を水色の枠で囲み、入る位置に線を出す
    if (fileDropIndex >= 0)
    {
        g.setColour (Theme::accent.withAlpha (0.6f));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (4.0f), 8.0f, 2.0f);

        const int y = dropLineY (fileDropIndex);
        g.setColour (Theme::accent);
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) padX, (float) y - 2.0f, (float) (getWidth() - padX * 2), 4.0f), 2.0f);
        return;
    }

    if (draggingRow == nullptr || dropIndex < 0)
        return;

    // 動かさない位置（元の位置のすぐ上・すぐ下）には線を出さない
    if (dropIndex == dragFromIndex || dropIndex == dragFromIndex + 1)
        return;

    const int y = dropLineY (dropIndex);
    g.setColour (Theme::accent);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) padX, (float) y - 2.0f, (float) (getWidth() - padX * 2), 4.0f), 2.0f);
}

//==============================================================================
// エクスプローラーからの .vst3 のドロップ

namespace
{
bool isVst3 (const juce::String& path)
{
    return path.endsWithIgnoreCase (".vst3");
}
} // namespace

bool ChainView::isInterestedInFileDrag (const juce::StringArray& files)
{
    // プラグインを順に読み込んでいる間（並びが一時停止）は受け取らない。画面を無効にしても、JUCE はファイルのドロップを通すため
    if (! isEnabled() || chain.isSuspended())
        return false;

    for (const auto& file : files)
        if (isVst3 (file))
            return true;

    return false;
}

void ChainView::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    fileDragMove (files, x, y);
}

void ChainView::fileDragMove (const juce::StringArray&, int, int y)
{
    const int index = insertIndexAt (y);
    if (index != fileDropIndex)
    {
        fileDropIndex = index;
        repaint();
    }
}

void ChainView::fileDragExit (const juce::StringArray&)
{
    fileDropIndex = -1;
    repaint();
}

void ChainView::filesDropped (const juce::StringArray& files, int, int y)
{
    const int index = insertIndexAt (y);
    fileDropIndex = -1;
    repaint();

    juce::StringArray plugins;
    for (const auto& file : files)
        if (isVst3 (file))
            plugins.add (file);

    // 読み込みには時間がかかることがあるので、ドロップの処理を終えてから行う
    if (! plugins.isEmpty() && callbacks.addPluginFiles != nullptr)
        juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<ChainView> (this), plugins, index]
        {
            if (safeThis != nullptr && ! safeThis->chain.isSuspended())
                safeThis->callbacks.addPluginFiles (plugins, index);
        });
}

//==============================================================================
void ChainView::rowDragStarted (PluginRow& row, const juce::MouseEvent& eventInRow)
{
    draggingRow = &row;
    dragFromIndex = chain.indexOf (row.getPluginId());
    dragOffsetY = eventInRow.getMouseDownPosition().y; // つかんだ位置を保ったまま動かす
    dropIndex = dragFromIndex;
    row.setDragging (true);
    row.toFront (false);
    beginDragAutoRepeat (30); // 端で止まっていても自動スクロールを続ける
    repaint();
}

void ChainView::rowDragged (const juce::MouseEvent& eventInThis)
{
    if (draggingRow == nullptr || rows.empty())
        return;

    // 行をマウスに合わせて動かす（少し右にずらして浮いて見せる）
    const int top = juce::jlimit (slotTop (0) - rowHeight / 2, slotTop ((int) rows.size() - 1) + rowHeight / 2,
                                  eventInThis.getPosition().y - dragOffsetY);
    draggingRow->setBounds (rowBounds (dragFromIndex).translated (12, 0).withY (top));

    // 行の中心がどの行より上にあるかで入る位置を決める
    dropIndex = insertIndexAt (top + rowHeight / 2);

    if (auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        const auto position = viewport->getLocalPoint (this, eventInThis.getPosition());
        viewport->autoScroll (position.x, position.y, 40, 14);
    }

    repaint();
}

void ChainView::rowDragEnded()
{
    if (draggingRow == nullptr)
        return;

    const int id = draggingRow->getPluginId();
    const int from = dragFromIndex;
    const int to = dropIndex;

    draggingRow->setDragging (false);
    draggingRow = nullptr;
    dropIndex = -1;

    if (to >= 0 && to != from && to != from + 1)
        chain.movePlugin (id, to > from ? to - 1 : to); // 並びが変わると syncRows で並べ直される
    else
        resized();

    repaint();
}
