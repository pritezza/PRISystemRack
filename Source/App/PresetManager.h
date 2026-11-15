#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChainPresets.h"

// プリセットの管理（並べ替え・名前の変更・削除）をする画面。
// - 左のつまみを上下にドラッグして並べ替える
// - 名前をダブルクリックして名前を変える（Enter で決定、Esc で取り消し）
// - [削除] で消す（確認あり）
// 変えるたびにすぐファイルに保存し、onChanged で知らせる（メイン画面の選択欄を作り直す）
class PresetManager final : public juce::Component
{
public:
    PresetManager (ChainPresets& presetsToEdit, std::function<void()> onChangedCallback);
    ~PresetManager() override;

    // ダイアログとして開く（閉じるまでほかの操作はできない）
    static void show (ChainPresets& presets, std::function<void()> onChanged);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Row;
    class List;

    void rebuild();
    void rowDragStarted (Row& row);
    void rowDragged (const juce::MouseEvent& eventInList);
    void rowDragEnded();
    int insertIndexAt (int y) const;
    void confirmDelete (const juce::String& id, const juce::String& name);
    void rename (const juce::String& id, const juce::String& newName);

    ChainPresets& presets;
    std::function<void()> onChanged;

    std::unique_ptr<List> list;
    juce::Viewport viewport;
    juce::Label hintLabel, emptyLabel;
    juce::TextButton closeButton;

    std::vector<std::unique_ptr<Row>> rows;
    Row* draggingRow = nullptr;
    int dragFromIndex = -1;
    int dropIndex = -1;
    int dragOffsetY = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};
