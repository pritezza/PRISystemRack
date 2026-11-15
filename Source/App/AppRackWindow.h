#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AppRack.h"
#include "ChainController.h"
#include "ChainView.h"

// アプリ用ラック 1 つ分の画面: 名前、対象アプリ、状態表示、プラグインの並び（合流先はメインの出力の手前に固定）
class AppRackComponent final : public juce::Component,
                               private juce::ChangeListener,
                               private juce::Timer
{
public:
    // onDeleteRequested: 「ラックを削除」が確認されたら呼ばれる（呼び出し元がウィンドウとラックを消す）
    AppRackComponent (AppRack& rackToUse, std::function<void (int rackId)> onDeleteRequested);
    ~AppRackComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onNameChanged; // ウィンドウのタイトルを変えるため

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void syncFromRack();
    void rebuildTargetChips();
    void updateStatus();
    void confirmDelete();

    AppRack& rack;
    std::function<void (int)> deleteRequested;
    ChainController controller;

    juce::Label nameLabel, targetsLabel;
    juce::TextEditor nameEditor;
    juce::TextButton bypassButton, deleteButton, chooseAppsButton;
    std::vector<std::unique_ptr<juce::TextButton>> targetChips;
    juce::Label captureStatus, outputStatus;
    juce::Viewport viewport;
    ChainView chainView;
    int timerTicks = 0;

    // ツールチップの表示役（この画面の子にして倍率に合わせる）
    juce::TooltipWindow tooltipWindow { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRackComponent)
};

// アプリ用ラックのウィンドウ。閉じても隠すだけで、ラックは動き続ける（メイン画面の一覧から開き直す）
class AppRackWindow final : public juce::DocumentWindow
{
public:
    AppRackWindow (AppRack& rack, std::function<void (int rackId)> onDeleteRequested);

    void closeButtonPressed() override;
    void showAndFocus();
    void applyUiScale(); // 表示の大きさ（Theme::effectiveScale()）が変わったら呼ぶ

private:
    void updateTitle();

    AppRack& rack;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRackWindow)
};
