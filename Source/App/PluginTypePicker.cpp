#include "PluginTypePicker.h"

#include "DialogLaunch.h"
#include "ScaledContent.h"
#include "Theme.h"
#include "WindowRendering.h"

namespace
{
// 中身を調べるのがこれより長くかかるときだけ画面を出す（普通のプラグインは別のプロセスで調べても約 1 秒なので出さない）
constexpr int showAfterMs = 1500;
const juce::Colour selectedColour { 0xff1e4257 };

juce::String titleFor (const juce::File& file)
{
    return TRANS ("Choose a plugin in %file%").replace ("%file%", file.getFileName());
}
} // namespace

// まだ画面に出していない選択（調べている途中）。出すときに画面へ移し、出さずに終わったらここで消す。
// アプリの終了時には JUCE が先に消す（DeletedAtShutdown）
class PluginTypePicker::Pending final : public juce::DeletedAtShutdown
{
public:
    ~Pending() override { clearSingletonInstance(); }

    juce::OwnedArray<PluginTypePicker> pickers;

    JUCE_DECLARE_SINGLETON_SINGLETHREADED_MINIMAL_INLINE (Pending)
};

namespace
{
juce::OwnedArray<PluginTypePicker>& pendingPickers()
{
    return PluginTypePicker::Pending::getInstance()->pickers;
}
} // namespace

//==============================================================================
void PluginTypePicker::choose (const juce::File& file, Callback onResult)
{
    // 保存してある結果で 1 つに決まる（または読み込めないと分かる）なら、画面は出さない
    juce::Array<juce::PluginDescription> cached;
    if (PluginScanner::findCached (file, cached) && cached.size() <= 1)
    {
        Result result;
        if (cached.isEmpty())
        {
            result.kind = Result::Kind::failed;
            result.error = TRANS ("Cannot load this file as a VST3 plugin: %path%").replace ("%path%", file.getFullPathName());
        }
        else
        {
            result.kind = Result::Kind::chosen;
            result.description = cached.getReference (0);
        }
        onResult (result);
        return;
    }

    pendingPickers().add (new PluginTypePicker (file, std::move (onResult)));
}

PluginTypePicker::PluginTypePicker (const juce::File& fileToScan, Callback onResult)
    : file (fileToScan), callback (std::move (onResult))
{
    messageLabel.setColour (juce::Label::textColourId, Theme::subText);
    messageLabel.setFont (juce::FontOptions (Theme::bodySize));
    messageLabel.setMinimumHorizontalScale (1.0f); // 横に縮めず、入りきらなければ折り返す
    messageLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (messageLabel);

    searchEditor.setTextToShowWhenEmpty (TRANS ("Search by name"), Theme::dimText);
    searchEditor.setFont (juce::FontOptions (Theme::bodySize));
    searchEditor.onTextChange = [this] { updateFilter(); };
    searchEditor.onReturnKey = [this] { chooseSelected(); };
    addChildComponent (searchEditor);

    list.setModel (this);
    list.setRowHeight (34);
    list.setColour (juce::ListBox::backgroundColourId, Theme::rackBackground);
    list.setColour (juce::ListBox::outlineColourId, Theme::cardBorder);
    list.setOutlineThickness (1);
    addChildComponent (list);

    cancelButton.setButtonText (TRANS ("Cancel"));
    Theme::styleButton (cancelButton);
    cancelButton.onClick = [this] { finish ({}); };
    addAndMakeVisible (cancelButton);

    okButton.setButtonText ("OK");
    okButton.setColour (juce::TextButton::buttonColourId, Theme::on);
    okButton.setColour (juce::ComboBox::outlineColourId, Theme::accent);
    okButton.onClick = [this] { chooseSelected(); };
    okButton.setEnabled (false);
    addChildComponent (okButton);

    setSize (560, 600);

    juce::Array<juce::PluginDescription> cached;
    if (PluginScanner::findCached (file, cached))
    {
        showTypes (cached);
    }
    else
    {
        messageLabel.setText (TRANS ("Scanning %file%...").replace ("%file%", file.getFileName()), juce::dontSendNotification);
        scanning = true;
        scanner.start (file);
    }
    startTimer (100);
}

PluginTypePicker::~PluginTypePicker()
{
    // 結果を渡さずに閉じられた（ウィンドウの × など）ときは、取り消しとして渡す（閉じている最中に次の処理が動かないよう後で）
    if (! reported && callback != nullptr)
        juce::MessageManager::callAsync ([cb = callback] { cb ({}); });
}

//==============================================================================
void PluginTypePicker::timerCallback()
{
    const bool shown = findParentComponentOfClass<juce::DialogWindow>() != nullptr;

    if (scanning)
    {
        juce::Array<juce::PluginDescription> found;
        juce::String error;
        const auto state = scanner.poll (found, error);

        if (state == PluginScanner::State::scanning)
        {
            messageLabel.setText (TRANS ("Scanning %file%... %seconds% s").replace ("%file%", file.getFileName())
                                          .replace ("%seconds%", juce::String ((int) scanner.getElapsedSeconds()))
                                      + "\n\n"
                                      + TRANS ("Files that contain many plugins, such as Waves WaveShell, can take a minute the first time. The result is saved, so the next time is instant."),
                                  juce::dontSendNotification);
            if (! shown && scanner.getElapsedSeconds() * 1000.0 >= showAfterMs)
                present();
            return;
        }

        scanning = false;

        if (state == PluginScanner::State::failed)
        {
            finish ({ Result::Kind::failed, {}, error });
            return;
        }

        if (found.isEmpty())
        {
            finish ({ Result::Kind::failed, {}, TRANS ("Cannot load this file as a VST3 plugin: %path%").replace ("%path%", file.getFullPathName()) });
            return;
        }

        if (found.size() == 1)
        {
            finish ({ Result::Kind::chosen, found.getReference (0), {} });
            return;
        }

        showTypes (found);
    }

    if (! shown && ! types.isEmpty())
        present();
    else if (! scanning)
        stopTimer(); // 一覧を出した。あとは選ばれるのを待つだけ
}

void PluginTypePicker::present()
{
    stopTimer();

    auto& pending = pendingPickers();
    const int index = pending.indexOf (this);
    if (index < 0)
        return;

    std::unique_ptr<PluginTypePicker> self (pending.removeAndReturn (index));

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ScaledContent (std::move (self), Theme::effectiveScale()));
    options.dialogTitle = titleFor (file);
    options.dialogBackgroundColour = Theme::windowBackground;
    options.useNativeTitleBar = true;
    options.resizable = true;

    if (auto* window = DialogLaunch::launchAsync (options))
        WindowRendering::install (*window);

    if (scanning)
        startTimer (100); // 調べ終わるのを待ち続ける
    else if (searchEditor.isVisible())
        searchEditor.grabKeyboardFocus();
}

void PluginTypePicker::showTypes (const juce::Array<juce::PluginDescription>& found)
{
    types = found;
    std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });

    messageLabel.setText (TRANS ("%file% contains %count% plugins. Choose the one to use.")
                              .replace ("%file%", file.getFileName())
                              .replace ("%count%", juce::String (types.size())),
                          juce::dontSendNotification);
    searchEditor.setVisible (true);
    list.setVisible (true);
    okButton.setVisible (true);
    updateFilter();
    resized();
    if (isShowing())
        searchEditor.grabKeyboardFocus();
}

void PluginTypePicker::updateFilter()
{
    const auto words = juce::StringArray::fromTokens (searchEditor.getText(), true);

    visible.clear();
    for (int i = 0; i < types.size(); ++i)
    {
        const auto& type = types.getReference (i);
        const auto text = type.name + " " + type.manufacturerName + " " + type.category;
        bool matches = true;
        for (const auto& word : words)
            matches = matches && text.containsIgnoreCase (word);
        if (matches)
            visible.add (i);
    }

    list.updateContent();
    list.selectRow (visible.isEmpty() ? -1 : 0);
    okButton.setEnabled (! visible.isEmpty());
    list.repaint();
}

void PluginTypePicker::chooseSelected()
{
    const int row = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (row, visible.size()))
        return;

    finish ({ Result::Kind::chosen, types.getReference (visible[row]), {} });
}

void PluginTypePicker::finish (const Result& result)
{
    if (reported)
        return;

    reported = true;
    stopTimer();
    scanner.cancel();

    const auto cb = callback;
    auto* window = findParentComponentOfClass<juce::DialogWindow>();

    // 「読み込んでいます」を表示してから結果を渡し、渡し終わってから閉じる（安全チェックと読み込みの進み具合は、
    // 結果を受けた側が出すダイアログ（PluginLoadDialog）が出す）
    if (window != nullptr && result.kind == Result::Kind::chosen)
    {
        messageLabel.setText (TRANS ("Loading %name%...").replace ("%name%", result.description.name), juce::dontSendNotification);
        searchEditor.setVisible (false);
        list.setVisible (false);
        okButton.setVisible (false);
        cancelButton.setEnabled (false);
        resized();

        // 読み込みは callAfterDelay の中で直接しない（PluginBrowser と同じ。プラグインが自分でメッセージを回すと
        // 同じタイマーがまた発火する）。タイマーでは 1 回きりのメッセージ（callAsync）を送るだけにして、タイマーが消えてから読み込む
        juce::Timer::callAfterDelay (150, [cb, result, safe = juce::Component::SafePointer<PluginTypePicker> (this)]
        {
            juce::MessageManager::callAsync ([cb, result, safe]
            {
                if (cb != nullptr)
                    cb (result);
                if (safe != nullptr)
                    if (auto* dialog = safe->findParentComponentOfClass<juce::DialogWindow>())
                        dialog->exitModalState (0); // ウィンドウと一緒にこの画面も消える
            });
        });
        return;
    }

    // 先に画面を閉じてから結果を渡す（結果を受けて次のファイルの選択を出したりするため）
    juce::MessageManager::callAsync ([cb, result] { if (cb != nullptr) cb (result); });

    if (window != nullptr)
    {
        window->exitModalState (0); // ウィンドウと一緒にこの画面も消える
        return;
    }

    // まだ画面に出していなかった。この後で消す（自分の処理の中で消さないよう）
    juce::MessageManager::callAsync ([self = this]
    {
        auto& pending = pendingPickers();
        if (pending.contains (self))
            pending.removeObject (self, true);
    });
}

//==============================================================================
int PluginTypePicker::getNumRows()
{
    return visible.size();
}

void PluginTypePicker::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow (row, visible.size()))
        return;

    const auto& type = types.getReference (visible[row]);

    if (rowIsSelected)
    {
        g.setColour (selectedColour);
        g.fillRect (0, 0, width, height);
    }
    g.setColour (Theme::divider);
    g.fillRect (0, height - 1, width, 1);

    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (Theme::bodySize, rowIsSelected ? juce::Font::bold : juce::Font::plain));
    g.drawText (type.name, 12, 0, width * 6 / 10 - 12, height, juce::Justification::centredLeft, true);

    g.setColour (Theme::subText);
    g.setFont (juce::FontOptions (Theme::smallSize));
    const auto detail = type.category.isNotEmpty() ? type.manufacturerName + "  |  " + type.category : type.manufacturerName;
    g.drawText (detail, width * 6 / 10, 0, width * 4 / 10 - 12, height, juce::Justification::centredRight, true);
}

void PluginTypePicker::listBoxItemDoubleClicked (int, const juce::MouseEvent&)
{
    chooseSelected();
}

void PluginTypePicker::returnKeyPressed (int)
{
    chooseSelected();
}

void PluginTypePicker::selectedRowsChanged (int lastRowSelected)
{
    okButton.setEnabled (lastRowSelected >= 0);
}

//==============================================================================
void PluginTypePicker::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBackground);
}

void PluginTypePicker::resized()
{
    auto area = getLocalBounds().reduced (16, 12);

    auto buttons = area.removeFromBottom (Theme::controlHeight);
    okButton.setBounds (buttons.removeFromRight (juce::jmax (80, Theme::buttonWidth (okButton))));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (Theme::buttonWidth (cancelButton)));
    area.removeFromBottom (10);

    messageLabel.setBounds (area.removeFromTop (list.isVisible() ? 26 : area.getHeight()));
    if (! list.isVisible())
        return;

    area.removeFromTop (6);
    searchEditor.setBounds (area.removeFromTop (Theme::controlHeight));
    area.removeFromTop (8);
    list.setBounds (area);
}
