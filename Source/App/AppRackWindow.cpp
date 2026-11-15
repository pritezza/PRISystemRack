#include "AppRackWindow.h"

#include "AlwaysOnTop.h"
#include "ProcessPicker.h"
#include "ScaledContent.h"
#include "Theme.h"
#include "WindowRendering.h"

namespace
{
const juce::Colour backgroundColour = Theme::windowBackground;
const juce::Colour okColour = Theme::ok;
const juce::Colour warningColour = Theme::warning;
const juce::Colour idleColour = Theme::subText;

void styleButton (juce::TextButton& button)
{
    Theme::styleButton (button);
}

void setStatus (juce::Label& label, const juce::String& text, juce::Colour colour)
{
    label.setText (text.isEmpty() ? juce::String() : juce::String::fromUTF8 ("\xe2\x97\x8f ") + text, juce::dontSendNotification); // ●
    label.setColour (juce::Label::textColourId, colour);
}

// 上の欄: 2 行（名前 / 対象アプリ）と状態表示 2 つ、行の間、上下の余白（合流先はメインの出力の手前に固定）
constexpr int headerPadY = 8;
constexpr int rowGap = 6;
constexpr int statusGap = 10;
constexpr int captureStatusHeight = 20;
constexpr int outputStatusHeight = 36; // 二重出力の警告などで 2 行になる
constexpr int headerHeight = headerPadY * 2 + Theme::controlHeight * 2 + rowGap + statusGap + captureStatusHeight + outputStatusHeight;
} // namespace

//==============================================================================
AppRackComponent::AppRackComponent (AppRack& rackToUse, std::function<void (int)> onDeleteRequested)
    : rack (rackToUse),
      deleteRequested (std::move (onDeleteRequested)),
      controller (rackToUse.getChain(), [this] { return rack.getName(); },
                  [this] { viewport.setViewPosition (0, chainView.getHeight()); }),
      chainView (rackToUse, controller.callbacks())
{
    for (auto* label : { &nameLabel, &targetsLabel })
    {
        label->setColour (juce::Label::textColourId, idleColour);
        label->setFont (juce::FontOptions (Theme::bodySize));
        label->setBorderSize ({ 1, 0, 1, 0 }); // 左右の内側の余白なし（見出しの列を文字の幅に詰めるため）
        addAndMakeVisible (label);
    }
    nameLabel.setText (TRANS ("Rack name"), juce::dontSendNotification);
    targetsLabel.setText (TRANS ("Target apps"), juce::dontSendNotification);

    nameEditor.setText (rack.getName(), juce::dontSendNotification);
    nameEditor.onTextChange = [this]
    {
        rack.setName (nameEditor.getText());
        if (onNameChanged != nullptr)
            onNameChanged();
    };
    addAndMakeVisible (nameEditor);

    bypassButton.setButtonText (TRANS ("Bypass"));
    bypassButton.setClickingTogglesState (true);
    styleButton (bypassButton);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, Theme::bypassOn);
    bypassButton.onClick = [this] { rack.getChain().setBypassAll (bypassButton.getToggleState()); };
    addAndMakeVisible (bypassButton);

    deleteButton.setButtonText (TRANS ("Delete rack"));
    styleButton (deleteButton);
    deleteButton.setColour (juce::TextButton::textColourOffId, Theme::error);
    deleteButton.onClick = [this] { confirmDelete(); };
    addAndMakeVisible (deleteButton);

    chooseAppsButton.setButtonText (TRANS ("Choose apps..."));
    styleButton (chooseAppsButton);
    chooseAppsButton.onClick = [this]
    {
        ProcessPicker::show (rack.getName(), rack.getTargets(),
                             [safeThis = juce::Component::SafePointer<AppRackComponent> (this)] (const juce::StringArray& targets)
                             {
                                 if (safeThis != nullptr)
                                     safeThis->rack.setTargets (targets);
                             });
    };
    addAndMakeVisible (chooseAppsButton);

    for (auto* label : { &captureStatus, &outputStatus })
    {
        label->setFont (juce::FontOptions (Theme::bodySize));
        label->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (label);
    }
    nameEditor.setFont (juce::FontOptions (Theme::bodySize));
    nameEditor.applyFontToAllText (juce::FontOptions (Theme::bodySize)); // すでに入っている文字にも当てる

    viewport.setViewedComponent (&chainView, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    rack.addChangeListener (this);
    syncFromRack();
    updateStatus();

    setSize (900, 820);
    startTimerHz (20);
}

AppRackComponent::~AppRackComponent()
{
    stopTimer();
    rack.removeChangeListener (this);
}

void AppRackComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncFromRack();
}

void AppRackComponent::syncFromRack()
{
    if (! nameEditor.hasKeyboardFocus (true))
        nameEditor.setText (rack.getName(), juce::dontSendNotification);

    bypassButton.setToggleState (rack.getChain().isBypassAll(), juce::dontSendNotification);
    bypassButton.setColour (juce::ComboBox::outlineColourId, rack.getChain().isBypassAll() ? warningColour : Theme::buttonBorder);

    rebuildTargetChips();
}

void AppRackComponent::rebuildTargetChips()
{
    const auto targets = rack.getTargets();

    // 同じなら作り直さない
    bool same = (int) targetChips.size() == targets.size();
    for (int i = 0; same && i < targets.size(); ++i)
        same = targetChips[(size_t) i]->getName() == targets[i];
    if (same)
        return;

    targetChips.clear();
    for (const auto& target : targets)
    {
        auto chip = std::make_unique<juce::TextButton> (target + "  x");
        chip->setName (target);
        chip->setTooltip (TRANS ("Remove %name% from the targets").replace ("%name%", target));
        styleButton (*chip);
        chip->onClick = [this, target]
        {
            auto remaining = rack.getTargets();
            remaining.removeString (target, true);
            rack.setTargets (remaining);
        };
        addAndMakeVisible (*chip);
        targetChips.push_back (std::move (chip));
    }
    resized();
}

void AppRackComponent::timerCallback()
{
    // メーターは ChainView が自分のタイマー（メーターの fps）で更新する
    if (++timerTicks % 10 == 0)
        updateStatus();
}

void AppRackComponent::updateStatus()
{
    const auto& status = rack.getStatus();

    if (rack.getTargets().isEmpty())
    {
        setStatus (captureStatus, TRANS ("Choose the apps to capture with \"Choose apps...\""), idleColour);
        setStatus (outputStatus, {}, idleColour);
        return;
    }

    if (status.captureError.isNotEmpty())
    {
        setStatus (captureStatus, TRANS ("Cannot capture: %error%").replace ("%error%", status.captureError), warningColour);
    }
    else if (! status.runningTargets.isEmpty())
    {
        auto text = TRANS ("Capturing: %apps% (%count% processes)  |  About %ms% ms until merged")
                        .replace ("%apps%", status.runningTargets.joinIntoString (", "))
                        .replace ("%count%", juce::String (status.processCount))
                        .replace ("%ms%", juce::String (juce::roundToInt (status.delayMs)));
        if (! status.waitingTargets.isEmpty())
            text << "  |  " << TRANS ("Waiting for %apps% to start").replace ("%apps%", status.waitingTargets.joinIntoString (", "));
        setStatus (captureStatus, text, okColour);
    }
    else
    {
        setStatus (captureStatus, TRANS ("Waiting for %apps% to start (capturing starts automatically)")
                                      .replace ("%apps%", status.waitingTargets.joinIntoString (", ")),
                   idleColour);
    }

    const auto apps = status.runningTargets.joinIntoString (", ");
    if (status.runningTargets.isEmpty() || status.activeOutputs.isEmpty())
        setStatus (outputStatus, TRANS ("Output device: not playing yet"), idleColour);
    else if (status.doubled)
        setStatus (outputStatus,
                   TRANS ("Output device: %apps% -> %device%. Both the original and the processed audio are heard. "
                          "Change the output device in the app to one you do not listen to, such as CABLE Input.")
                       .replace ("%apps%", apps)
                       .replace ("%device%", status.activeOutputs.joinIntoString (", ")),
                   warningColour);
    else
        setStatus (outputStatus, TRANS ("Output device: %apps% -> %device% (the original audio is not heard)")
                                     .replace ("%apps%", apps)
                                     .replace ("%device%", status.activeOutputs.joinIntoString (", ")),
                   okColour);
}

void AppRackComponent::confirmDelete()
{
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, TRANS ("Delete rack"),
                                        TRANS ("Delete %name% and its plugins?").replace ("%name%", rack.getName()),
                                        TRANS ("Delete"), TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<AppRackComponent> (this)] (int result)
                                        {
                                            if (result == 0 || safeThis == nullptr)
                                                return;
                                            const int id = safeThis->rack.getId();
                                            // このウィンドウごと消えるので、ボタンの処理が終わってから行う
                                            juce::MessageManager::callAsync ([callback = safeThis->deleteRequested, id]
                                            {
                                                if (callback != nullptr)
                                                    callback (id);
                                            });
                                        }));
}

void AppRackComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (captureStatus.getBounds().getUnion (outputStatus.getBounds()).expanded (8, 4).toFloat(), 8.0f);
    g.setColour (Theme::divider);
    g.fillRect (0, viewport.getY() - 2, getWidth(), 2);
}

void AppRackComponent::resized()
{
    constexpr int headerPadX = 12;

    // 見出しの列は文字の幅に合わせる（「ラックの名前」「対象アプリ」の長い方 ＋ すき間）
    const juce::FontOptions labelFont (Theme::bodySize);
    int labelWidth = 0;
    for (const auto* label : { &nameLabel, &targetsLabel })
        labelWidth = juce::jmax (labelWidth, juce::GlyphArrangement::getStringWidthInt (labelFont, label->getText()));
    labelWidth += 12;

    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (headerPadX, headerPadY);

    auto row = header.removeFromTop (Theme::controlHeight);
    nameLabel.setBounds (row.removeFromLeft (labelWidth));
    deleteButton.setBounds (row.removeFromRight (Theme::buttonWidth (deleteButton)));
    row.removeFromRight (8);
    bypassButton.setBounds (row.removeFromRight (Theme::buttonWidth (bypassButton)));
    row.removeFromRight (12);
    nameEditor.setBounds (row.withWidth (juce::jmin (280, row.getWidth())));
    header.removeFromTop (rowGap);

    row = header.removeFromTop (Theme::controlHeight);
    targetsLabel.setBounds (row.removeFromLeft (labelWidth));
    for (auto& chip : targetChips)
    {
        const int width = juce::jmin (220, Theme::buttonWidth (*chip));
        chip->setBounds (row.removeFromLeft (width));
        row.removeFromLeft (8);
    }
    chooseAppsButton.setBounds (row.removeFromLeft (Theme::buttonWidth (chooseAppsButton)));
    header.removeFromTop (statusGap);

    captureStatus.setBounds (header.removeFromTop (captureStatusHeight).reduced (8, 0));
    outputStatus.setBounds (header.removeFromTop (outputStatusHeight).reduced (8, 0));

    area.removeFromTop (2);
    viewport.setBounds (area);
    chainView.setMinimumHeight (viewport.getHeight());
    chainView.setSize (viewport.getMaximumVisibleWidth(), chainView.getHeight());
}

//==============================================================================
AppRackWindow::AppRackWindow (AppRack& rackToUse, std::function<void (int)> onDeleteRequested)
    : DocumentWindow ({}, backgroundColour, DocumentWindow::allButtons,
                      false), // まだウィンドウを作らない（設定を済ませてから addToDesktop で 1 回だけ作る。PluginWindow.h と同じ理由）
      rack (rackToUse)
{
    setUsingNativeTitleBar (true);
    auto content = std::make_unique<AppRackComponent> (rack, std::move (onDeleteRequested));
    content->onNameChanged = [this] { updateTitle(); };
    setContentOwned (new ScaledContent (std::move (content), Theme::effectiveScale()), true);
    setResizable (true, true);
    setResizeLimits (juce::roundToInt (Theme::minWindowWidth * Theme::effectiveScale()), juce::roundToInt (Theme::minWindowHeight * Theme::effectiveScale()), 8000, 6000);
    ScaledContent::fitToScreen (*this);
    centreWithSize (getWidth(), getHeight());
    updateTitle();
    addToDesktop(); // ここで 1 回だけ作る（まだ表示しない）
    AlwaysOnTop::applyTo (*this);
}

void AppRackWindow::applyUiScale()
{
    if (auto* scaled = dynamic_cast<ScaledContent*> (getContentComponent()))
        scaled->rescaleWindow (*this, Theme::effectiveScale(), Theme::minWindowWidth, Theme::minWindowHeight);
}

void AppRackWindow::updateTitle()
{
    setName (TRANS ("App rack - %name%").replace ("%name%", rack.getName()));
}

void AppRackWindow::showAndFocus()
{
    setVisible (true);
    toFront (true);

    WindowRendering::install (*this); // 普段は Direct2D（文字がくっきり）、枠のドラッグ中だけソフトウェア描画
}

void AppRackWindow::closeButtonPressed()
{
    setVisible (false); // 閉じても隠すだけ。ラックは動き続ける
}
