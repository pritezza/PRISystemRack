#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginScanner.h"

// プラグインを探す VST3 フォルダー（%APPDATA%\PRISystemRack\PluginFolders.xml。なければ C:\Program Files\Common Files\VST3）
namespace PluginFolders
{
juce::StringArray load();
void save (const juce::StringArray& folders);
} // namespace PluginFolders

// プラグイン選択画面（［＋ プラグインを追加］と「別のプラグインに置き換える...」で出す）。
// VST3 フォルダーの .vst3 をすべて並べる。Waves の WaveShell のように 1 つのファイルに多数入っているものは、中のプラグインを
// 1 つずつ並べる。調べた結果は PluginScanner が保存しているので、2 回目からはすぐ出る。まだ調べていないファイル
// （初めて開いたとき、新しく入れたプラグイン）は、開いている間に別のプロセスで 3 つずつ調べ、終わったものから並べる。
// 下に［ファイルから追加...］（ファイルを選ぶ。今までの動き）、［VST3 フォルダー...］（探すフォルダーの管理）、
// ［一覧を作り直す］（すべて調べ直す）。結果はかならず 1 回 onResult に渡す（取り消し・閉じたときも）
class PluginBrowser final : public juce::Component,
                            private juce::ListBoxModel,
                            private juce::Timer
{
public:
    struct Result
    {
        enum class Kind
        {
            chosen,   // 一覧から選んだ（description）
            fromFile, // ［ファイルから追加...］を押した（呼び出し元がファイルを選ぶ画面を出す）
            cancelled,
        };

        Kind kind = Kind::cancelled;
        juce::PluginDescription description;
    };

    using Callback = std::function<void (const Result&)>;

    // title: ウィンドウのタイトル。actionText: 決めるボタンの文字（「追加」「置き換える」）
    static void show (const juce::String& title, const juce::String& actionText, Callback onResult);

    PluginBrowser (const juce::String& actionText, Callback onResult);
    ~PluginBrowser() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    int getMinimumWidth() const; // 下のボタンがすべて並ぶ幅（倍率をかける前。メイン画面の最小の幅以上）

    // 前回閉じたときに選んでいたプラグイン（PluginDescription::createIdentifierString()）と、その行が一覧の表示の上端から
    // 何ピクセル下にあったか。次に開いたとき、その行を選んで同じ位置に出す（追加と置き換えで共通。
    // 設定の lastBrowserPlugin / lastBrowserOffset。MainComponent が読み書きする）
    static void setLastSelection (const juce::String& identifier, int offsetFromTop) { lastIdentifier = identifier; lastOffset = offsetFromTop; }
    static juce::String getLastIdentifier() { return lastIdentifier; }
    static int getLastOffset() { return lastOffset; }

    // 一覧の並び（検索欄の右の選択欄。設定の browserSort。MainComponent が読み書きする）
    enum class SortOrder
    {
        name = 0,  // プラグイン名の順（同じ名前はファイルの順）
        maker = 1, // メーカー名の順（同じメーカーの中はプラグイン名の順）
    };
    static void setSortOrder (SortOrder order) { sortOrder = order; }
    static SortOrder getSortOrder() { return sortOrder; }

    // エフェクトのみ（インストゥルメントを一覧に出さない。設定の browserEffectsOnly。既定は ON）
    static void setEffectsOnly (bool shouldShowEffectsOnly) { effectsOnly = shouldShowEffectsOnly; }
    static bool isEffectsOnly() { return effectsOnly; }

private:
    inline static juce::String lastIdentifier;
    inline static int lastOffset = 0;
    inline static SortOrder sortOrder = SortOrder::name;
    inline static bool effectsOnly = true;

    void sortItems();

    void rememberSelection();   // 閉じるときに、選んでいる行と位置を覚える
    void restoreSelection();    // 前回の行が一覧にあれば選ぶ（まだ調べ終わっていなければ、出てきたときに選ぶ）
    void applyRestoredScroll(); // 選んだ行を前回と同じ位置に出す（一覧の大きさが決まってから）
    juce::String restoreIdentifier; // まだ選べていない前回の行（選べたら、または自分で選んだら空）
    int restoreOffset = 0;
    int restoredRow = -1;           // 位置を合わせる行（合わせたら -1）
    bool restoring = false;
    static constexpr int parallelScans = 3; // 同時に調べるファイルの数（別のプロセス）

    void timerCallback() override;
    void buildList (bool rescanAll); // フォルダーのファイルを集め直す。rescanAll なら保存してある結果を使わずすべて調べ直す
    void addTypes (const juce::Array<juce::PluginDescription>& found);
    void updateFilter();
    void updateMessage();
    void chooseSelected();
    void finish (const Result& result); // 結果を渡して画面を閉じる
    void editFolders();

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    juce::String getTooltipForRow (int row) override; // 安全チェックで読み込めないと分かったものは、その理由
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int lastRowSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    Callback callback;
    bool reported = false;

    juce::Array<juce::PluginDescription> items; // 一覧（sortOrder の順）
    juce::Array<int> visible;                   // 絞り込んだあとに出している items の番号

    // 調べるファイルの順番待ちと、調べている所
    juce::Array<juce::File> queue;
    std::array<PluginScanner, parallelScans> scanners;
    std::array<juce::File, parallelScans> scanningFiles;
    int scanTotal = 0, scanDone = 0;
    juce::Array<juce::File> failedFiles; // 調べられなかったファイル（上の文をクリックすると、ファイルごとの理由を出す）

    void mouseUp (const juce::MouseEvent&) override; // 上の文（messageLabel）のクリック
    void showFailures();

    juce::Label messageLabel;
    juce::TextEditor searchEditor;
    juce::ComboBox sortBox; // 並び（名前順 / メーカー順）
    juce::ToggleButton effectsOnlyToggle; // エフェクトのみ
    juce::ListBox list;
    juce::TextButton fileButton, foldersButton, rescanButton, cancelButton, okButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBrowser)
};
