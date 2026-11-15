#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginScanner.h"

// VST3 ファイルの中から使うプラグインを 1 つ決める。
// 中が 1 つならそのまま、複数（Waves の WaveShell など）なら一覧から選ばせる。初めてのファイルは先に中身を調べる
// （その間は経過時間と「取り消す」を出す）。結果はかならず 1 回 onResult に渡す（取り消し・閉じたときも）
class PluginTypePicker final : public juce::Component,
                               private juce::ListBoxModel,
                               private juce::Timer
{
public:
    struct Result
    {
        enum class Kind
        {
            chosen,
            failed,
            cancelled,
        };

        Kind kind = Kind::cancelled;
        juce::PluginDescription description; // chosen のとき
        juce::String error;                  // failed のとき
    };

    using Callback = std::function<void (const Result&)>;

    // file のプラグインを決める。保存してある結果で 1 つに決まれば、画面を出さずにこの中で onResult を呼ぶ。
    // 中身を調べるのが 1.5 秒より長くかかるとき、または複数入っているときだけ画面を出す
    static void choose (const juce::File& file, Callback onResult);

    PluginTypePicker (const juce::File& file, Callback onResult);
    ~PluginTypePicker() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    class Pending; // 画面に出す前の選択を持っておく所

private:
    void timerCallback() override;
    void present(); // 画面（ダイアログ）に出す
    void showTypes (const juce::Array<juce::PluginDescription>& found);
    void updateFilter();
    void finish (const Result& result); // 結果を渡して画面を閉じる
    void chooseSelected();

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int lastRowSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    const juce::File file;
    Callback callback;
    bool reported = false;

    PluginScanner scanner;
    bool scanning = false;
    juce::Array<juce::PluginDescription> types;
    juce::Array<int> visible; // 絞り込んだあとに出している types の番号

    juce::Label messageLabel;
    juce::TextEditor searchEditor;
    juce::ListBox list;
    juce::TextButton okButton, cancelButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginTypePicker)
};
