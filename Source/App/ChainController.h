#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChainView.h"
#include "PluginChain.h"
#include "PluginWindow.h"

// プラグインの並び 1 つ分の操作（プラグイン画面の開閉、ファイルの選択、追加・更新・置き換え・複製・削除）。
// メインのラックとアプリ用ラックで同じものを使う。ChainView にはこの callbacks() を渡す。
class ChainController final : private juce::ChangeListener
{
public:
    // windowTitlePrefix: プラグイン画面のタイトルの前に付ける（アプリ用ラックの名前など。空ならプラグイン名だけ）
    // onPluginAdded: ボタンで足したあとに呼ばれる（足した行が見えるようにスクロールするなど）
    ChainController (PluginChain& chainToUse, std::function<juce::String()> windowTitlePrefix, std::function<void()> onPluginAdded);
    ~ChainController() override;

    ChainView::Callbacks callbacks();

    void openEditor (int pluginId);
    void closeEditor (int pluginId);
    void openAllEditors();
    void closeAllEditors();

    void choosePlugin();
    void addPluginFiles (const juce::StringArray& files, int index);
    void reloadPlugin (int pluginId);
    void replacePlugin (int pluginId);
    void duplicatePlugin (int pluginId);
    void removePlugin (int pluginId);

private:
    // まとめて足すファイル（1 つずつ、中のどのプラグインを使うか決めてから足す）
    struct AddBatch
    {
        juce::StringArray files;
        int next = 0;
        int index = -1; // 足す位置（-1 なら末尾）
        bool fromButton = false;
        int added = 0;
        int lastAddedId = 0;
        juce::StringArray errors;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void choosePluginFile (const juce::String& title, const juce::File& initial, std::function<void (const juce::File&)> onChosen);
    void startAdding (const juce::StringArray& files, int index, bool fromButton);
    void addNextFile (std::shared_ptr<AddBatch> batch);
    void replaceWith (int pluginId, const juce::PluginDescription& description); // 置き換えて画面を開く
    void editorFaulted (int pluginId); // プラグインの画面の中で例外が起きた。画面を閉じて知らせる
    bool destroyEditorWindow (std::unique_ptr<PluginWindow> window); // 画面の窓を消して後始末する。消す途中で例外が起きたら false

public:
    void addChosen (const juce::PluginDescription& description); // 選択画面で選んだもの（起動オプションでも使う）を末尾に足して画面を開く

    PluginChain& chain;
    std::function<juce::String()> titlePrefix;
    std::function<void()> pluginAdded;

    std::map<int, std::unique_ptr<PluginWindow>> editorWindows;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true); // 非同期の呼び出しが、消えたあとに届かないように

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainController)
};
