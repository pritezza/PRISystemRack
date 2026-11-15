#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "RackModel.h"

// 信号の流れを上から下へ並べる画面: 入力 → プラグイン（並んでいる順）→ ＋ プラグインを追加 → 出力（アプリ用ラックでは合流）。
// メインのラックでは、出力の欄の下に、そこで合流するアプリ用ラックの名前を出す。
//
// - プラグインの行は、行のどこかを上下にドラッグして並べ替える（左端にドラッグできることを示すつまみの印を出す）
// - ON/OFF で処理させるかを切り替える。行のダブルクリックか [画面] でプラグインの画面を開く
// - ⋯ メニュー（行の右クリックでも出る）: 画面を開く、複製、プラグインを更新、別のプラグインに置き換える、削除
//
// 高さは中身に合わせて決まるので、Viewport に入れて使う（setMinimumHeight で表示領域の高さを渡す）。
class ChainView final : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    struct Callbacks
    {
        // エクスプローラーからドロップされた .vst3 を index の位置に入れる（行数と同じなら末尾）
        std::function<void (const juce::StringArray& files, int index)> addPluginFiles;
        std::function<void (int pluginId)> openEditor;
        std::function<void (int pluginId)> removePlugin;
        std::function<void (int pluginId)> duplicatePlugin;
        std::function<void (int pluginId)> reloadPlugin;
        std::function<void (int pluginId)> replacePlugin;
        std::function<void()> addPlugin;
    };

    ChainView (RackModel& modelToUse, Callbacks callbacksToUse);
    ~ChainView() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    void setMinimumHeight (int height);
    // 入力・出力の欄のメーターのピークホールド（すべての画面に共通。表示メニューで切り替える）
    static void setPeakHoldEnabled (bool enabled);
    static bool isPeakHoldEnabled();

    // 入力・出力の欄のメーターの種類（SectionMeter::toId の名前。入力・出力の欄のすべてで共通）。
    // 種類とピークホールドはメーターの上のホイールか右クリックのメニューでも変わり、そのたびに setSectionMeterChangedCallback の関数が呼ばれる
    static void setSectionMeterKind (const juce::String& id);
    static juce::String getSectionMeterKind();
    static void setSectionMeterChangedCallback (std::function<void()> callback);

    // メーター（入力・出力の欄とプラグインの行）を 1 秒に何回更新するか（すべての画面に共通。表示メニューの「メーターの fps」）。
    // 各画面が自分のタイマーで更新するので、変えると次の回から効く
    static void setMeterFramesPerSecond (int fps);
    static int getMeterFramesPerSecond();

    // FileDragAndDropTarget（.vst3 のファイルやフォルダーを受け付ける）
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    class PluginRow;
    class GainSection;

    // PluginRow から呼ぶ
    void rowDragStarted (PluginRow& row, const juce::MouseEvent& eventInRow);
    void rowDragged (const juce::MouseEvent& eventInThis);
    void rowDragEnded();
    PluginChain& getChain() { return chain; }
    const Callbacks& getCallbacks() const { return callbacks; }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override; // メーターの更新（getMeterFramesPerSecond() 回/秒）
    void updateMeters();
    void syncRows();
    void updateHeight();

    int contentHeight() const;
    int inputTop() const;  // 入力の欄の上端
    int addTop() const;    // 「＋ プラグインを追加」の上端
    int outputTop() const; // 出力（合流）の欄の上端
    int outputHeight() const; // 出力の欄の高さ
    int slotTop (int index) const; // index 番目の行の上端
    juce::Rectangle<int> rowBounds (int index) const;
    int dropLineY (int insertIndex) const;
    int insertIndexAt (int centreY) const; // この高さに置いたら何番目に入るか

    RackModel& model;
    PluginChain& chain;
    Callbacks callbacks;
    std::vector<RackModel::MergedRack> mergedRacks; // 出力の手前で合流するアプリ用ラック（「＋ プラグインを追加」の左に名前の札を並べ、出力の欄へ矢印でつなぐ）

    std::unique_ptr<GainSection> inputSection, outputSection;
    std::vector<std::unique_ptr<PluginRow>> rows; // チェーンの並びと同じ順
    juce::TextButton addButton;
    int minimumHeight = 0;
    juce::uint32 lastRestingCheckMs = 0, lastLatencyCheckMs = 0;

    // 並べ替えのドラッグ
    PluginRow* draggingRow = nullptr;
    int dragFromIndex = -1;
    int dragOffsetY = 0;
    int dropIndex = -1; // 入る位置（0 = 先頭、行数 = 末尾）

    // ファイルのドロップ（ドラッグ中だけ 0 以上）
    int fileDropIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainView)
};
