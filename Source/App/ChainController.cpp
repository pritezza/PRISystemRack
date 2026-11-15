#include "ChainController.h"

#include "EditorGuard.h"
#include "PluginBrowser.h"
#include "PluginGuard.h"
#include "PluginLoadDialog.h"
#include "PluginSafety.h"
#include "PluginTypePicker.h"

namespace
{
const juce::File defaultPluginFolder ("C:\\Program Files\\Common Files\\VST3");
}

ChainController::ChainController (PluginChain& chainToUse, std::function<juce::String()> windowTitlePrefix,
                                  std::function<void()> onPluginAdded)
    : chain (chainToUse), titlePrefix (std::move (windowTitlePrefix)), pluginAdded (std::move (onPluginAdded))
{
    chain.addChangeListener (this);
}

ChainController::~ChainController()
{
    *alive = false;
    chain.removeChangeListener (this);
    closeAllEditors();
}

ChainView::Callbacks ChainController::callbacks()
{
    return {
        [this] (const juce::StringArray& files, int index) { addPluginFiles (files, index); },
        [this] (int id) { openEditor (id); },
        [this] (int id) { removePlugin (id); },
        [this] (int id) { duplicatePlugin (id); },
        [this] (int id) { reloadPlugin (id); },
        [this] (int id) { replacePlugin (id); },
        [this] { choosePlugin(); },
    };
}

void ChainController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // 削除されたプラグインと、エラーで止まったプラグインの画面を閉じる
    // （止まったプラグインの画面は、中で処理側と同じロックを待って固まることがあるため）
    for (auto it = editorWindows.begin(); it != editorWindows.end();)
    {
        const auto* plugin = chain.findPlugin (it->first);
        if (plugin == nullptr || plugin->faulted.load())
        {
            auto window = std::move (it->second);
            it = editorWindows.erase (it);
            window->disableSnapshots();
            destroyEditorWindow (std::move (window));
        }
        else
            ++it;
    }
}

//==============================================================================
void ChainController::openEditor (int pluginId)
{
    auto* plugin = chain.findPlugin (pluginId);

    if (plugin == nullptr || plugin->instance == nullptr || plugin->faulted.load())
        return;

    if (auto it = editorWindows.find (pluginId); it != editorWindows.end())
    {
        it->second->toFront (true);
        return;
    }

    // 画面を作る（プラグインの中で例外が起きても落ちないよう包む。DodgePro は画面を作る途中で 0 で割って落ちた）
    const auto name = plugin->getDisplayName();
    auto showOpenError = [name] (bool restartRecommended)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot open editor"),
                                                TRANS ("An error occurred while %name% was creating its editor, so the editor could not be opened. "
                                                       "The plugin keeps processing the sound.").replace ("%name%", name)
                                                    + (restartRecommended ? "\n\n" + TRANS ("Restarting the application is recommended.") : juce::String()));
    };

    juce::AudioProcessorEditor* editor = nullptr;
    if (! PluginGuard::createEditor (plugin->instance.get(), &editor))
    {
        showOpenError (false);
        return;
    }

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*plugin->instance);

    const auto prefix = titlePrefix != nullptr ? titlePrefix() : juce::String();
    const auto title = prefix.isNotEmpty() ? prefix + " - " + name : name;

    // 前に開いていた位置（と大きさ）で開き、動かしたらプラグインごとに覚えておく（設定ファイルに保存される）。
    // 表示すると JUCE がプラグインに画面を取り付ける（attached）ので、窓を作る処理も包む。途中で例外が起きたら、
    // 作りかけの窓には触らずに手放す（解放すると壊れた状態のプラグインを呼ぶため）
    std::unique_ptr<PluginWindow> window;
    const bool windowCreated = EditorGuard::callGuarded ([&] { window = std::make_unique<PluginWindow> (
        editor, title, plugin->editorBounds,
        [this, pluginId] (juce::Rectangle<int> bounds)
        {
            if (auto* p = chain.findPlugin (pluginId))
                p->editorBounds = bounds;
        },
        [this, weak = std::weak_ptr<bool> (alive), pluginId]
        {
            juce::MessageManager::callAsync ([this, weak, pluginId]
            {
                if (auto flag = weak.lock(); flag != nullptr && *flag)
                    closeEditor (pluginId);
            });
        },
        [this, pluginId] (const juce::Image& image)
        {
            // 撮った画面を行の表示用に控える（縮小して JPEG にし、設定とプリセットに保存される）
            if (auto* p = chain.findPlugin (pluginId); p != nullptr && ! p->faulted.load())
                p->setSnapshot (image);
        },
        [this, weak = std::weak_ptr<bool> (alive), pluginId]
        {
            if (auto flag = weak.lock(); flag != nullptr && *flag)
                editorFaulted (pluginId);
        }); });

    if (! windowCreated || window == nullptr)
    {
        showOpenError (true);
        return;
    }

    editorWindows[pluginId] = std::move (window);
}

// プラグインの画面の中で例外が起きた（EditorGuard が受け止めた）。アプリが落ちないよう画面を閉じて知らせる。
// プラグインの処理（音）は続ける。画面を消す途中でまた例外が起きたら、その窓は解放せずに手放す（壊れた状態に触らないため）
void ChainController::editorFaulted (int pluginId)
{
    auto it = editorWindows.find (pluginId);
    if (it == editorWindows.end())
        return;

    auto window = std::move (it->second);
    editorWindows.erase (it);
    window->disableSnapshots();
    const bool destroyed = destroyEditorWindow (std::move (window));

    const auto* plugin = chain.findPlugin (pluginId);
    const auto name = plugin != nullptr ? plugin->getDisplayName() : juce::String();
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Plugin editor closed"),
                                            TRANS ("An error occurred in the editor of %name%, so it was closed to keep the application running. "
                                                   "The plugin keeps processing the sound.").replace ("%name%", name)
                                                + "\n\n"
                                                + (destroyed ? TRANS ("If it happens again, use ... > \"Reload plugin\" or restart the application.")
                                                             : TRANS ("The editor could not be closed cleanly. Restarting the application is recommended.")));
}

void ChainController::closeEditor (int pluginId)
{
    auto it = editorWindows.find (pluginId);
    if (it == editorWindows.end())
        return;

    auto window = std::move (it->second);
    editorWindows.erase (it);
    destroyEditorWindow (std::move (window));
}

// 画面の窓を消す（どの閉じ方でもここを通す）。例外が起きた画面は、消す処理も包む（プラグインがまた落ちてもアプリが止まらないように。
// 途中で例外が起きたら、その窓は解放せずに手放し、false を返す）。どちらも、消したあとで EditorGuard の後始末をする
// （例外が起きた画面は、消す間も包んだままにするので、~PluginWindow では後始末しない。ここで済ませないと状態が残り、
// 次に同じアドレスに作られた画面がそれを使ってしまう）
bool ChainController::destroyEditorWindow (std::unique_ptr<PluginWindow> window)
{
    if (window == nullptr)
        return true;

    auto* raw = window.release();
    bool destroyed = true;
    if (EditorGuard::hasFaulted (*raw))
    {
        raw->disableSnapshots();
        destroyed = EditorGuard::callGuarded ([raw]
        {
            raw->setVisible (false);
            delete raw;
        });
    }
    else
    {
        delete raw;
    }
    EditorGuard::release (raw); // 消したあとなので、アドレスで探して後始末するだけ
    return destroyed;
}

void ChainController::openAllEditors()
{
    for (const auto& plugin : chain.getPlugins())
        openEditor (plugin->id);
}

void ChainController::closeAllEditors()
{
    auto windows = std::move (editorWindows);
    editorWindows.clear();
    for (auto& [id, window] : windows)
        destroyEditorWindow (std::move (window));
}

//==============================================================================
void ChainController::choosePluginFile (const juce::String& title, const juce::File& initial,
                                        std::function<void (const juce::File&)> onChosen)
{
    fileChooser = std::make_unique<juce::FileChooser> (title, initial, "*.vst3");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [weak = std::weak_ptr<bool> (alive), onChosen] (const juce::FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();
                                  if (auto flag = weak.lock(); flag != nullptr && *flag && file != juce::File())
                                      onChosen (file);
                              });
}

// ［＋ プラグインを追加］: プラグイン選択画面を出す。［ファイルから追加...］ならファイルを選ぶ（今までの動き）
void ChainController::choosePlugin()
{
    PluginBrowser::show (TRANS ("Add plugin"), TRANS ("Add"), [this, weak = std::weak_ptr<bool> (alive)] (const PluginBrowser::Result& result)
    {
        if (auto flag = weak.lock(); flag == nullptr || ! *flag)
            return;

        if (result.kind == PluginBrowser::Result::Kind::chosen)
            addChosen (result.description);
        else if (result.kind == PluginBrowser::Result::Kind::fromFile)
            choosePluginFile (TRANS ("Choose a VST3 plugin"), defaultPluginFolder, [this] (const juce::File& file)
            {
                startAdding ({ file.getFullPathName() }, -1, true);
            });
    });
}

// 選択画面で選んだプラグインを末尾に足して、画面を開く（ボタンで足したときと同じ）。
// 安全チェック（補助プログラムで試す）と本体への読み込みは、進み具合のダイアログを出しながら行う（PluginLoadDialog）
void ChainController::addChosen (const juce::PluginDescription& description)
{
    if (chain.isSuspended()) // プラグインを順に読み込んでいる途中（選ぶ画面を開いたまま読み込みが始まったときなど）
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot load plugin"), TRANS ("Plugins are still being loaded. Try again after loading finishes."));
        return;
    }
    if (chain.isFull()) // 上限なら、安全チェック（別プロセスで試しに読む）もしない
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot load plugin"), PluginChain::describeFull());
        return;
    }

    auto addedId = std::make_shared<int> (0);
    const std::weak_ptr<bool> weak (alive);
    PluginLoadDialog::run (description,
        [this, weak, description, addedId] (juce::String& error)
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return false;
            // 確かめている間（最大 120 秒）に、プリセットを開くなどして順の読み込みが始まっていたら足さない
            if (chain.isSuspended())
            {
                error = TRANS ("Plugins are still being loaded. Try again after loading finishes.");
                return false;
            }
            if (auto* plugin = chain.addPlugin (description, error, -1))
            {
                *addedId = plugin->id;
                return true;
            }
            error = description.name + ": " + error;
            return false;
        },
        [this, weak, addedId] (bool loaded, bool cancelled, const juce::String& error)
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;
            if (loaded)
            {
                if (pluginAdded != nullptr)
                    pluginAdded();
                openEditor (*addedId);
            }
            else if (! cancelled && error.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot load plugin"), error);
            }
        });
}

void ChainController::addPluginFiles (const juce::StringArray& files, int index)
{
    startAdding (files, index, false);
}

void ChainController::startAdding (const juce::StringArray& files, int index, bool fromButton)
{
    auto batch = std::make_shared<AddBatch>();
    batch->files = files;
    batch->index = index;
    batch->fromButton = fromButton;
    addNextFile (batch);
}

// ファイルを 1 つずつ、中のどのプラグインを使うか決めてから足す（WaveShell などは一覧から選ぶ。初めてのファイルは中身を調べる）
void ChainController::addNextFile (std::shared_ptr<AddBatch> batch)
{
    // プラグインを順に読み込んでいる途中なら、残りのファイルは足さない（読み込みと重なると「変更なし」の基準に入ってしまうため）
    if (batch->next < batch->files.size() && chain.isSuspended())
    {
        batch->errors.add (TRANS ("Plugins are still being loaded. Try again after loading finishes."));
        batch->next = batch->files.size();
    }

    // 上限に達したら残りのファイルは足さない
    if (batch->next < batch->files.size() && chain.isFull())
    {
        batch->errors.add (PluginChain::describeFull());
        batch->next = batch->files.size();
    }

    if (batch->next >= batch->files.size())
    {
        if (batch->added > 0 && batch->fromButton && pluginAdded != nullptr)
            pluginAdded();

        // 1 つだけならボタンで足したときと同じく画面を開く（複数のときは画面が並ばないよう開かない）
        if (batch->added == 1)
            openEditor (batch->lastAddedId);

        if (! batch->errors.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot load plugin"),
                                                    batch->errors.joinIntoString ("\n"));
        return;
    }

    const juce::File file (batch->files[batch->next++]);

    PluginTypePicker::choose (file, [this, weak = std::weak_ptr<bool> (alive), batch, file] (const PluginTypePicker::Result& result)
    {
        if (auto flag = weak.lock(); flag == nullptr || ! *flag)
            return;

        if (result.kind == PluginTypePicker::Result::Kind::chosen)
        {
            // 安全チェックと本体への読み込みは、進み具合のダイアログを出しながら行う。取りやめたら残りのファイルも足さない
            PluginLoadDialog::run (result.description,
                [this, weak, batch, file, description = result.description] (juce::String& error)
                {
                    if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                        return false;
                    if (chain.isSuspended()) // 確かめている間に順の読み込みが始まっていたら足さない
                    {
                        error = TRANS ("Plugins are still being loaded. Try again after loading finishes.");
                        return false;
                    }
                    if (auto* plugin = chain.addPlugin (description, error, batch->index))
                    {
                        batch->lastAddedId = plugin->id;
                        ++batch->added;
                        if (batch->index >= 0)
                            ++batch->index; // 複数なら、選んだ順にその位置から並べる（末尾に足すときは末尾のまま）
                        return true;
                    }
                    error = file.getFileName() + ": " + error;
                    return false;
                },
                [this, weak, batch] (bool loaded, bool cancelled, const juce::String& error)
                {
                    if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                        return;
                    if (cancelled)
                        batch->next = batch->files.size();
                    else if (! loaded && error.isNotEmpty())
                        batch->errors.add (error);
                    addNextFile (batch);
                });
            return;
        }
        else if (result.kind == PluginTypePicker::Result::Kind::failed)
        {
            batch->errors.add (file.getFileName() + ": " + result.error);
        }

        addNextFile (batch);
    });
}

void ChainController::reloadPlugin (int pluginId)
{
    const auto* plugin = chain.findPlugin (pluginId);
    if (plugin == nullptr)
        return;

    // 安全チェックで読み込まないと分かっているものは、そのままでは何度押しても同じ理由で断られる。
    // 読み込みのダイアログで理由を出して「もう一度チェックしますか？」と聞き、チェックし直してから読み込む
    if (PluginSafety::isBlocked (plugin->description))
    {
        const std::weak_ptr<bool> weak (alive);
        PluginLoadDialog::run (plugin->description,
            [this, weak, pluginId] (juce::String& error)
            {
                if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                    return false;
                if (chain.findPlugin (pluginId) == nullptr)
                    return false; // 確かめている間に消された
                if (chain.isSuspended()) // 確かめている間に順の読み込みが始まっていたら読み込み直さない
                {
                    error = TRANS ("Plugins are still being loaded. Try again after loading finishes.");
                    return false;
                }
                closeEditor (pluginId); // プラグインより先に画面を閉じる（読み込めていないので普通は開いていない）
                return chain.reloadPlugin (pluginId, error);
            },
            [weak] (bool loaded, bool cancelled, const juce::String& error)
            {
                if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                    return;
                if (! loaded && ! cancelled && error.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot reload"), error);
            });
        return;
    }

    const bool editorWasOpen = editorWindows.count (pluginId) != 0;
    closeEditor (pluginId); // プラグインより先に画面を閉じる

    juce::String error;
    if (chain.reloadPlugin (pluginId, error))
    {
        if (editorWasOpen)
            openEditor (pluginId);
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot reload"), error);
    }
}

void ChainController::replacePlugin (int pluginId)
{
    const auto* plugin = chain.findPlugin (pluginId);

    if (plugin == nullptr)
        return;

    const juce::File current (plugin->description.fileOrIdentifier);
    const auto initial = current.existsAsFile() ? current : defaultPluginFolder;
    const auto name = plugin->getDisplayName();

    // プラグイン選択画面を出す。［ファイルから追加...］ならファイルを選ぶ（今までの動き）
    PluginBrowser::show (TRANS ("Replace %name% with another plugin").replace ("%name%", name), TRANS ("Replace"),
                         [this, weak = std::weak_ptr<bool> (alive), pluginId, initial, name] (const PluginBrowser::Result& result)
    {
        if (auto flag = weak.lock(); flag == nullptr || ! *flag)
            return;

        if (result.kind == PluginBrowser::Result::Kind::chosen)
        {
            replaceWith (pluginId, result.description);
            return;
        }

        if (result.kind != PluginBrowser::Result::Kind::fromFile)
            return;

        choosePluginFile (TRANS ("Choose a plugin to replace %name%").replace ("%name%", name), initial,
                          [this, pluginId] (const juce::File& file)
                          {
                              // 中のどのプラグインを使うか決めてから置き換える（WaveShell などは一覧から選ぶ）
                              PluginTypePicker::choose (file, [this, weak = std::weak_ptr<bool> (alive), pluginId] (const PluginTypePicker::Result& picked)
                              {
                                  if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                                      return;

                                  if (picked.kind == PluginTypePicker::Result::Kind::failed)
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot replace"), picked.error);
                                  else if (picked.kind == PluginTypePicker::Result::Kind::chosen)
                                      replaceWith (pluginId, picked.description);
                              });
                          });
    });
}

void ChainController::replaceWith (int pluginId, const juce::PluginDescription& description)
{
    if (chain.findPlugin (pluginId) == nullptr)
        return;

    // 安全チェックと本体への読み込みは、進み具合のダイアログを出しながら行う（PluginLoadDialog）。
    // 置き換えたら、足したときと同じく新しいプラグインの画面を開く（行のエディターの画面も撮り直される）。
    // 失敗したときは元のプラグインのままなので、画面が開いていたときだけ開き直す
    auto editorWasOpen = std::make_shared<bool> (false);
    const std::weak_ptr<bool> weak (alive);
    PluginLoadDialog::run (description,
        [this, weak, pluginId, description, editorWasOpen] (juce::String& error)
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return false;
            if (chain.findPlugin (pluginId) == nullptr)
                return false; // 確かめている間に消された
            if (chain.isSuspended()) // 確かめている間に順の読み込みが始まっていたら置き換えない
            {
                error = TRANS ("Plugins are still being loaded. Try again after loading finishes.");
                return false;
            }
            *editorWasOpen = editorWindows.count (pluginId) != 0;
            closeEditor (pluginId);
            return chain.replacePlugin (pluginId, description, error);
        },
        [this, weak, pluginId, editorWasOpen] (bool replaced, bool cancelled, const juce::String& error)
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;
            if ((replaced || *editorWasOpen) && chain.findPlugin (pluginId) != nullptr)
                openEditor (pluginId);
            if (! replaced && ! cancelled && error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot replace"), error);
        });
}

void ChainController::duplicatePlugin (int pluginId)
{
    juce::String error;
    if (chain.duplicatePlugin (pluginId, error) == nullptr)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Cannot duplicate"), error);
}

void ChainController::removePlugin (int pluginId)
{
    closeEditor (pluginId); // プラグインより先に画面を閉じる
    chain.removePlugin (pluginId);
}
