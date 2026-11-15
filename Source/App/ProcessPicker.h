#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "HostCore/ProcessTools.h"

// アプリ用ラックの対象アプリ（exe）を選ぶ画面。行のクリックで選択／解除（複数可）。
// 起動していないアプリも名前で登録できる。OK で onAccepted に選んだ exe 名を渡す
class ProcessPicker final : public juce::Component,
                            private juce::ListBoxModel
{
public:
    ProcessPicker (const juce::StringArray& currentTargets, std::function<void (const juce::StringArray&)> onAccepted);

    // ダイアログとして開く
    static void show (const juce::String& rackName, const juce::StringArray& currentTargets,
                      std::function<void (const juce::StringArray&)> onAccepted);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::String exeName;
        int processCount = 0;
        juce::StringArray activeEndpoints;
        bool running = false;
    };

    void refresh();
    void addByName();
    void close (bool accepted);

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;

    juce::StringArray selected;
    std::function<void (const juce::StringArray&)> accepted;
    std::vector<Row> rows;

    juce::Label hintLabel;
    juce::ToggleButton audioOnlyToggle;
    juce::TextButton refreshButton, addButton, okButton, cancelButton;
    juce::ListBox list;
    juce::Label nameLabel;
    juce::TextEditor nameEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessPicker)
};
