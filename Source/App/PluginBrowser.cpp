#include "PluginBrowser.h"

#include "DialogLaunch.h"
#include "PluginSafety.h"
#include "ScaledContent.h"
#include "Theme.h"
#include "WindowRendering.h"

namespace
{
const juce::Colour selectedColour { 0xff1e4257 };

juce::File foldersFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("PRISystemRack")
        .getChildFile ("PluginFolders.xml");
}

juce::String defaultFolder()
{
    return "C:\\Program Files\\Common Files\\VST3";
}

// 決めるボタン（水色）の見た目
void styleDefaultButton (juce::TextButton& button)
{
    button.setColour (juce::TextButton::buttonColourId, Theme::on);
    button.setColour (juce::ComboBox::outlineColourId, Theme::accent);
}

// minWidth / minHeight: 小さくできる限度（倍率をかける前。メイン画面と同じく、それより小さくはできない）
void launchDialog (std::unique_ptr<juce::Component> content, const juce::String& title, int minWidth, int minHeight)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ScaledContent (std::move (content), Theme::effectiveScale()));
    options.dialogTitle = title;
    options.dialogBackgroundColour = Theme::windowBackground;
    options.useNativeTitleBar = true;
    options.resizable = true;

    if (auto* window = DialogLaunch::launchAsync (options))
    {
        const float scale = Theme::effectiveScale();
        window->setResizeLimits (juce::roundToInt ((float) minWidth * scale), juce::roundToInt ((float) minHeight * scale), 8000, 6000);
        WindowRendering::install (*window);
    }
}

void closeDialogOf (juce::Component& component)
{
    if (auto* window = component.findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0); // ウィンドウと一緒に中身も消える
}

//==============================================================================
// 探す VST3 フォルダーの管理（一覧、［フォルダーを追加...］、［削除］、［閉じる］）。変えるたびにすぐ保存する
class PluginFolderEditor final : public juce::Component,
                                 private juce::ListBoxModel
{
public:
    explicit PluginFolderEditor (std::function<void (bool changed)> onClosed)
        : closed (std::move (onClosed)), folders (PluginFolders::load())
    {
        messageLabel.setText (TRANS ("Plugins are searched for in these folders and their subfolders."), juce::dontSendNotification);
        messageLabel.setColour (juce::Label::textColourId, Theme::subText);
        messageLabel.setFont (juce::FontOptions (Theme::bodySize));
        addAndMakeVisible (messageLabel);

        list.setModel (this);
        list.setRowHeight (27);
        list.setColour (juce::ListBox::backgroundColourId, Theme::rackBackground);
        list.setColour (juce::ListBox::outlineColourId, Theme::cardBorder);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        addButton.setButtonText (TRANS ("Add folder..."));
        Theme::styleButton (addButton);
        addButton.onClick = [this] { addFolder(); };
        addAndMakeVisible (addButton);

        removeButton.setButtonText (TRANS ("Remove"));
        Theme::styleButton (removeButton);
        removeButton.onClick = [this] { removeSelected(); };
        removeButton.setEnabled (false);
        addAndMakeVisible (removeButton);

        closeButton.setButtonText (TRANS ("Close"));
        styleDefaultButton (closeButton);
        closeButton.onClick = [this] { closeDialogOf (*this); };
        addAndMakeVisible (closeButton);

        setSize (Theme::minWindowWidth, Theme::minWindowHeight);
    }

    ~PluginFolderEditor() override
    {
        // 閉じたら（ボタンでもウィンドウの × でも）知らせる。閉じている最中に次の処理が動かないよう後で
        if (closed != nullptr)
            juce::MessageManager::callAsync ([cb = closed, changedNow = changed] { cb (changedNow); });
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::windowBackground); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        auto buttons = area.removeFromBottom (Theme::controlHeight);
        closeButton.setBounds (buttons.removeFromRight (juce::jmax (80, Theme::buttonWidth (closeButton))));
        addButton.setBounds (buttons.removeFromLeft (Theme::buttonWidth (addButton)));
        buttons.removeFromLeft (8);
        removeButton.setBounds (buttons.removeFromLeft (Theme::buttonWidth (removeButton)));
        area.removeFromBottom (10);
        messageLabel.setBounds (area.removeFromTop (26));
        area.removeFromTop (6);
        list.setBounds (area);
    }

private:
    int getNumRows() override { return folders.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (! juce::isPositiveAndBelow (row, folders.size()))
            return;
        if (rowIsSelected)
        {
            g.setColour (selectedColour);
            g.fillRect (0, 0, width, height);
        }
        const bool exists = juce::File (folders[row]).isDirectory();
        g.setColour (exists ? Theme::text : Theme::dimText);
        g.setFont (juce::FontOptions (Theme::bodySize));
        g.drawText (folders[row] + (exists ? juce::String() : TRANS (" (not found)")), 10, 0, width - 20, height,
                    juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int lastRowSelected) override { removeButton.setEnabled (lastRowSelected >= 0); }

    void addFolder()
    {
        chooser = std::make_unique<juce::FileChooser> (TRANS ("Choose a folder that contains VST3 plugins"), juce::File (defaultFolder()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<PluginFolderEditor> (this)] (const juce::FileChooser& fc)
                              {
                                  const auto folder = fc.getResult();
                                  if (safe == nullptr || folder == juce::File())
                                      return;
                                  if (! safe->folders.contains (folder.getFullPathName(), true))
                                  {
                                      safe->folders.add (folder.getFullPathName());
                                      safe->changed = true;
                                      PluginFolders::save (safe->folders);
                                      safe->list.updateContent();
                                      safe->list.repaint();
                                  }
                              });
    }

    void removeSelected()
    {
        const int row = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, folders.size()))
            return;
        folders.remove (row);
        changed = true;
        PluginFolders::save (folders);
        list.deselectAllRows();
        list.updateContent();
        list.repaint();
    }

    std::function<void (bool)> closed;
    juce::StringArray folders;
    bool changed = false;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Label messageLabel;
    juce::ListBox list;
    juce::TextButton addButton, removeButton, closeButton;
};
} // namespace

//==============================================================================
namespace PluginFolders
{
juce::StringArray load()
{
    juce::StringArray folders;
    if (const auto xml = juce::parseXMLIfTagMatches (foldersFile(), "PLUGINFOLDERS"))
        for (auto* child : xml->getChildWithTagNameIterator ("FOLDER"))
            folders.addIfNotAlreadyThere (child->getStringAttribute ("path"), true);
    else
        folders.add (defaultFolder()); // まだ保存していない（初めて）

    folders.removeEmptyStrings();
    return folders;
}

void save (const juce::StringArray& folders)
{
    juce::XmlElement root ("PLUGINFOLDERS");
    for (const auto& folder : folders)
        root.createNewChildElement ("FOLDER")->setAttribute ("path", folder);
    foldersFile().getParentDirectory().createDirectory();
    root.writeTo (foldersFile());
}
} // namespace PluginFolders

//==============================================================================
void PluginBrowser::show (const juce::String& title, const juce::String& actionText, Callback onResult)
{
    auto browser = std::make_unique<PluginBrowser> (actionText, std::move (onResult));
    const int minWidth = browser->getMinimumWidth();
    juce::Component::SafePointer<PluginBrowser> safeBrowser (browser.get());
    launchDialog (std::move (browser), title, minWidth, Theme::minWindowHeight); // 縦はメイン画面の最小と同じ

    // 開いたらすぐ名前を打てるよう、検索欄にカーソルを置く（窓が出てから）
    juce::MessageManager::callAsync ([safeBrowser]
    {
        if (safeBrowser != nullptr)
            safeBrowser->searchEditor.grabKeyboardFocus();
    });
}

// 下のボタンがすべて並ぶ幅（メイン画面の最小の幅より狭くはしない）
int PluginBrowser::getMinimumWidth() const
{
    int buttons = 16 * 2 + juce::jmax (90, Theme::buttonWidth (okButton)) + 8 + Theme::buttonWidth (cancelButton) + 24;
    for (const auto* button : { &fileButton, &foldersButton, &rescanButton })
        buttons += Theme::buttonWidth (*button) + 8;
    return juce::jmax (Theme::minWindowWidth, buttons);
}

PluginBrowser::PluginBrowser (const juce::String& actionText, Callback onResult)
    : callback (std::move (onResult))
{
    messageLabel.setColour (juce::Label::textColourId, Theme::subText);
    messageLabel.setFont (juce::FontOptions (Theme::smallSize));
    messageLabel.setMinimumHorizontalScale (1.0f);
    messageLabel.setJustificationType (juce::Justification::centredLeft);
    messageLabel.addMouseListener (this, false); // 調べられなかったファイルがあるとき、クリックで理由を出す（mouseUp）
    addAndMakeVisible (messageLabel);

    searchEditor.setTextToShowWhenEmpty (TRANS ("Search by name, maker or category"), Theme::dimText);
    searchEditor.setFont (juce::FontOptions (Theme::bodySize));
    searchEditor.onTextChange = [this] { updateFilter(); };
    searchEditor.onReturnKey = [this] { chooseSelected(); };
    addAndMakeVisible (searchEditor);

    // 並び。変えたら並べ直し、選んでいる行が見えるようにする
    sortBox.addItem (TRANS ("Sort by name"), 1 + (int) SortOrder::name);
    sortBox.addItem (TRANS ("Sort by maker"), 1 + (int) SortOrder::maker);
    sortBox.setSelectedId (1 + (int) sortOrder, juce::dontSendNotification);
    sortBox.setTitle (TRANS ("Sort order"));
    sortBox.onChange = [this]
    {
        sortOrder = (SortOrder) (sortBox.getSelectedId() - 1);
        addTypes ({}); // 並べ直す（選んでいる行は保つ）
        if (const int row = list.getSelectedRow(); row >= 0)
            list.scrollToEnsureRowIsOnscreen (row);
    };
    addAndMakeVisible (sortBox);

    // エフェクトのみ（インストゥルメントを出さない。このアプリでは音を作るものは使い道がないので既定は ON）
    effectsOnlyToggle.setButtonText (TRANS ("Effects only"));
    effectsOnlyToggle.setTooltip (TRANS ("Hide instruments (synths and samplers). They make no sound in this app because it has no MIDI input."));
    effectsOnlyToggle.setToggleState (effectsOnly, juce::dontSendNotification);
    effectsOnlyToggle.onClick = [this]
    {
        effectsOnly = effectsOnlyToggle.getToggleState();
        addTypes ({}); // 出し直す（選んでいる行は、残っていれば保つ）
        updateMessage();
        if (const int row = list.getSelectedRow(); row >= 0)
            list.scrollToEnsureRowIsOnscreen (row);
    };
    addAndMakeVisible (effectsOnlyToggle);

    list.setModel (this);
    list.setRowHeight (37);
    list.setColour (juce::ListBox::backgroundColourId, Theme::rackBackground);
    list.setColour (juce::ListBox::outlineColourId, Theme::cardBorder);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    fileButton.setButtonText (TRANS ("Add from file..."));
    fileButton.setTooltip (TRANS ("Choose a .vst3 file directly (for plugins outside the VST3 folders)"));
    fileButton.onClick = [this] { finish ({ Result::Kind::fromFile, {} }); };

    foldersButton.setButtonText (TRANS ("VST3 folders..."));
    foldersButton.setTooltip (TRANS ("Choose the folders that are searched for plugins"));
    foldersButton.onClick = [this] { editFolders(); };

    rescanButton.setButtonText (TRANS ("Rebuild list"));
    rescanButton.setTooltip (TRANS ("Scan all plugins in the VST3 folders again (files that contain many plugins, such as Waves WaveShell, take a while)"));
    rescanButton.onClick = [this] { buildList (true); };

    cancelButton.setButtonText (TRANS ("Cancel"));
    cancelButton.onClick = [this] { finish ({}); };

    for (auto* button : { &fileButton, &foldersButton, &rescanButton, &cancelButton })
    {
        Theme::styleButton (*button);
        addAndMakeVisible (*button);
    }

    okButton.setButtonText (actionText);
    styleDefaultButton (okButton);
    okButton.onClick = [this] { chooseSelected(); };
    okButton.setEnabled (false);
    addAndMakeVisible (okButton);

    setSize (720, 640);
    restoreIdentifier = lastIdentifier;
    restoreOffset = lastOffset;
    buildList (false);
}

PluginBrowser::~PluginBrowser()
{
    if (! reported)
        rememberSelection();

    // 結果を渡さずに閉じられた（ウィンドウの × など）ときは、取り消しとして渡す（閉じている最中に次の処理が動かないよう後で）
    if (! reported && callback != nullptr)
        juce::MessageManager::callAsync ([cb = callback] { cb ({}); });
}

//==============================================================================
void PluginBrowser::buildList (bool rescanAll)
{
    stopTimer();
    for (auto& scanner : scanners)
        scanner.cancel();
    for (auto& file : scanningFiles)
        file = juce::File();

    // 選んでいる行は、作り直したあとに出てきたら選び直す（restoreSelection と同じ仕組み）。
    // 一覧（items）を空にするときは、表示している行の番号（visible）も一緒に空にする（古い番号のまま items を読むと範囲外になる。
    // 2026-11-15、行を選んだまま［一覧を作り直す］を押すと落ちていた）
    if (const int row = list.getSelectedRow(); juce::isPositiveAndBelow (row, visible.size()) && juce::isPositiveAndBelow (visible[row], items.size()))
        restoreIdentifier = items.getReference (visible[row]).createIdentifierString();
    items.clear();
    visible.clear();
    list.deselectAllRows();
    list.updateContent();
    queue.clear();
    scanTotal = scanDone = 0;
    failedFiles.clear();

    // フォルダーの中の .vst3（ファイルとフォルダー形式の両方。下のフォルダーも）を集める
    juce::FileSearchPath path;
    for (const auto& folder : PluginFolders::load())
        path.add (juce::File (folder));

    juce::VST3PluginFormat format;
    for (const auto& identifier : format.searchPathsForPlugins (path, true, false))
    {
        const juce::File file (identifier);
        juce::Array<juce::PluginDescription> cached;
        if (! rescanAll && PluginScanner::findCached (file, cached))
            items.addArray (cached);
        else if (! rescanAll && PluginScanner::isKnownFailure (file))
            failedFiles.add (file);
        else
            queue.add (file);
    }
    scanTotal = queue.size();

    addTypes ({}); // 並べ直して出す
    updateMessage();
    if (! queue.isEmpty())
        startTimer (100);
}

void PluginBrowser::timerCallback()
{
    bool busy = false;

    for (size_t i = 0; i < scanners.size(); ++i)
    {
        juce::Array<juce::PluginDescription> found;
        juce::String error;
        const auto state = scanners[i].poll (found, error);

        if (state == PluginScanner::State::scanning)
        {
            busy = true;
            continue;
        }

        if (state == PluginScanner::State::done || state == PluginScanner::State::failed)
        {
            ++scanDone;
            if (state == PluginScanner::State::failed)
                failedFiles.add (scanningFiles[i]);
            else
                addTypes (found);
            scanningFiles[i] = juce::File();
        }

        // 空いたら次のファイルを調べ始める
        if (! queue.isEmpty())
        {
            scanningFiles[i] = queue.removeAndReturn (0);
            scanners[i].start (scanningFiles[i]);
            busy = true;
        }
    }

    if (! busy && queue.isEmpty())
        stopTimer(); // 調べ終わった（先に止めて、表示を「%count% 個のプラグイン」にする）
    updateMessage();
}

void PluginBrowser::addTypes (const juce::Array<juce::PluginDescription>& found)
{
    // 選んでいる行を保つ
    juce::String selectedId;
    if (const int row = list.getSelectedRow(); juce::isPositiveAndBelow (row, visible.size()) && juce::isPositiveAndBelow (visible[row], items.size()))
        selectedId = items.getReference (visible[row]).createIdentifierString();

    items.addArray (found);
    sortItems();

    updateFilter();

    if (selectedId.isNotEmpty())
        for (int i = 0; i < visible.size(); ++i)
            if (items.getReference (visible[i]).createIdentifierString() == selectedId)
            {
                list.selectRow (i, true);
                break;
            }

    restoreSelection();
}

void PluginBrowser::sortItems()
{
    const bool byMaker = sortOrder == SortOrder::maker;
    std::stable_sort (items.begin(), items.end(), [byMaker] (const auto& a, const auto& b)
    {
        if (byMaker)
            if (const int maker = a.manufacturerName.compareNatural (b.manufacturerName); maker != 0)
                return maker < 0;
        const int byName = a.name.compareNatural (b.name);
        return byName != 0 ? byName < 0 : a.fileOrIdentifier.compareNatural (b.fileOrIdentifier) < 0;
    });
}

void PluginBrowser::rememberSelection()
{
    const int row = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (row, visible.size()))
        return;

    const int viewY = list.getViewport() != nullptr ? list.getViewport()->getViewPositionY() : 0;
    setLastSelection (items.getReference (visible[row]).createIdentifierString(), row * list.getRowHeight() - viewY);
}

void PluginBrowser::restoreSelection()
{
    if (restoreIdentifier.isEmpty() || searchEditor.getText().isNotEmpty())
        return;

    for (int i = 0; i < visible.size(); ++i)
        if (items.getReference (visible[i]).createIdentifierString() == restoreIdentifier)
        {
            const juce::ScopedValueSetter<bool> setting (restoring, true);
            list.selectRow (i, true); // 位置は applyRestoredScroll で合わせる
            restoreIdentifier = {};
            restoredRow = i;
            applyRestoredScroll();
            return;
        }
}

void PluginBrowser::applyRestoredScroll()
{
    auto* viewport = list.getViewport();
    if (restoredRow < 0 || viewport == nullptr || list.getHeight() <= 0)
        return;

    // 前回と同じく、表示の上端から restoreOffset 下に出す（一覧の端を越えるときは端に寄せる。少なくとも行は見えるように）
    const int rowY = restoredRow * list.getRowHeight();
    const int visibleHeight = viewport->getViewHeight();
    const int offset = juce::jlimit (0, juce::jmax (0, visibleHeight - list.getRowHeight()), restoreOffset);
    viewport->setViewPosition (0, juce::jmax (0, rowY - offset));
    restoredRow = -1;
}

void PluginBrowser::updateFilter()
{
    const auto words = juce::StringArray::fromTokens (searchEditor.getText(), true);

    visible.clear();
    for (int i = 0; i < items.size(); ++i)
    {
        const auto& type = items.getReference (i);
        if (effectsOnly && type.isInstrument) // 「エフェクトのみ」のときはインストゥルメントを出さない
            continue;
        const auto text = type.name + " " + type.manufacturerName + " " + type.category;
        bool matches = true;
        for (const auto& word : words)
            matches = matches && text.containsIgnoreCase (word);
        if (matches)
            visible.add (i);
    }

    list.updateContent();
    if (list.getSelectedRow() >= visible.size() || (list.getSelectedRow() < 0 && ! visible.isEmpty() && searchEditor.getText().isNotEmpty()))
        list.selectRow (visible.isEmpty() ? -1 : 0);
    okButton.setEnabled (juce::isPositiveAndBelow (list.getSelectedRow(), visible.size()));
    list.repaint();
}

void PluginBrowser::updateMessage()
{
    juce::String text;
    if (isTimerRunning() || ! queue.isEmpty())
    {
        juce::StringArray names;
        for (const auto& file : scanningFiles)
            if (file != juce::File())
                names.add (file.getFileNameWithoutExtension());

        text = TRANS ("Scanning the VST3 folders... %done% / %total%").replace ("%done%", juce::String (scanDone)).replace ("%total%", juce::String (scanTotal))
             + (names.isEmpty() ? juce::String() : "  (" + names.joinIntoString (", ") + ")") + "\n"
             + TRANS ("Plugins appear in the list as each file is scanned. Files that contain many plugins, such as Waves WaveShell, take a while the first time.");
    }
    else
    {
        text = TRANS ("%count% plugins").replace ("%count%", juce::String (items.size()));
        if (effectsOnly)
        {
            int instruments = 0;
            for (const auto& type : items)
                instruments += type.isInstrument ? 1 : 0;
            if (instruments > 0)
                text << "  " << TRANS ("(%count% instruments hidden)").replace ("%count%", juce::String (instruments));
        }
        if (! failedFiles.isEmpty())
            text << "  " << TRANS ("(%count% files could not be scanned. Click here to see why)").replace ("%count%", juce::String (failedFiles.size()));
    }

    if (text != messageLabel.getText())
        messageLabel.setText (text, juce::dontSendNotification);

    // 調べ終わって調べられなかったファイルがあるときだけ、クリックで理由を出す
    const bool clickable = ! failedFiles.isEmpty() && ! isTimerRunning() && queue.isEmpty();
    messageLabel.setMouseCursor (clickable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    messageLabel.setColour (juce::Label::textColourId, clickable ? Theme::warning : Theme::subText);
}

void PluginBrowser::mouseUp (const juce::MouseEvent& event)
{
    if (event.eventComponent == &messageLabel && ! failedFiles.isEmpty() && ! isTimerRunning() && queue.isEmpty())
        showFailures();
}

namespace
{
// 調べられなかったファイルの一覧の中身: 1 件ずつカード（プラグインの行と同じ色）にして、ファイル名（太字）、
// 理由（注意の色。幅に合わせて折り返す）、場所（淡い色）を縦に並べる。高さは幅で決まる（layoutForWidth）
class FailureCards final : public juce::Component
{
public:
    struct Entry
    {
        juce::String name, reason, path;
    };

    explicit FailureCards (juce::Array<Entry> list) : entries (std::move (list)) {}

    void layoutForWidth (int width)
    {
        cards.clear();
        int y = 0;
        for (const auto& entry : entries)
        {
            juce::AttributedString text;
            text.setWordWrap (juce::AttributedString::byWord);
            text.setLineSpacing (2.0f);
            text.append (entry.name + "\n", juce::FontOptions (Theme::bodySize, juce::Font::bold), Theme::text);
            text.append (entry.reason + "\n", juce::FontOptions (Theme::bodySize), Theme::warning);
            text.append (entry.path, juce::FontOptions (Theme::smallSize), Theme::dimText);

            Card card;
            card.layout.createLayout (text, (float) juce::jmax (1, width - padding * 2));
            const int height = (int) std::ceil (card.layout.getHeight()) + padding * 2;
            card.bounds = { 0, y, width, height };
            cards.push_back (std::move (card));
            y += height + gap;
        }
        setSize (width, juce::jmax (0, y - gap));
    }

    void paint (juce::Graphics& g) override
    {
        for (const auto& card : cards)
        {
            const auto area = card.bounds.toFloat().reduced (0.5f);
            g.setColour (Theme::card);
            g.fillRoundedRectangle (area, 6.0f);
            g.setColour (Theme::cardBorder);
            g.drawRoundedRectangle (area, 6.0f, 1.0f);
            card.layout.draw (g, card.bounds.reduced (padding).toFloat());
        }
    }

private:
    static constexpr int padding = 10, gap = 8;

    struct Card
    {
        juce::Rectangle<int> bounds;
        juce::TextLayout layout;
    };

    juce::Array<Entry> entries;
    std::vector<Card> cards;
};

// 調べられなかったファイルの一覧の画面（説明、カードの一覧（スクロール）、［すべてコピー］、［閉じる］）
class FailuresView final : public juce::Component
{
public:
    explicit FailuresView (juce::Array<FailureCards::Entry> entries) : cards (entries)
    {
        messageLabel.setText (TRANS ("\"Rebuild list\" scans them again after you fix the cause."), juce::dontSendNotification);
        messageLabel.setColour (juce::Label::textColourId, Theme::subText);
        messageLabel.setFont (juce::FontOptions (Theme::bodySize));
        addAndMakeVisible (messageLabel);

        viewport.setViewedComponent (&cards, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        // 問い合わせなどに貼れるよう、すべてを文字でコピーする
        for (const auto& entry : entries)
            plainText << entry.name << "\n" << entry.reason << "\n" << entry.path << "\n\n";
        copyButton.setButtonText (TRANS ("Copy all"));
        Theme::styleButton (copyButton);
        copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (plainText.trimEnd()); };
        addAndMakeVisible (copyButton);

        closeButton.setButtonText (TRANS ("Close"));
        styleDefaultButton (closeButton);
        closeButton.onClick = [this] { closeDialogOf (*this); };
        addAndMakeVisible (closeButton);

        setSize (640, juce::jlimit (Theme::minWindowHeight, 560, 130 + 92 * entries.size()));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::windowBackground); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        auto buttons = area.removeFromBottom (Theme::controlHeight);
        closeButton.setBounds (buttons.removeFromRight (juce::jmax (80, Theme::buttonWidth (closeButton))));
        copyButton.setBounds (buttons.removeFromLeft (Theme::buttonWidth (copyButton)));
        area.removeFromBottom (12);
        messageLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (8);
        viewport.setBounds (area);
        cards.layoutForWidth (viewport.getWidth() - viewport.getScrollBarThickness() - 4);
    }

private:
    juce::Label messageLabel;
    FailureCards cards;
    juce::Viewport viewport;
    juce::TextButton copyButton, closeButton;
    juce::String plainText;
};
} // namespace

void PluginBrowser::showFailures()
{
    juce::Array<FailureCards::Entry> entries;
    for (const auto& file : failedFiles)
        entries.add ({ file.getFileName(), PluginScanner::describeFailure (file), file.getFullPathName() });

    launchDialog (std::make_unique<FailuresView> (entries), TRANS ("Files that could not be scanned"), 420, 300);
}

void PluginBrowser::chooseSelected()
{
    const int row = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (row, visible.size()))
        return;

    finish ({ Result::Kind::chosen, items.getReference (visible[row]) });
}

void PluginBrowser::finish (const Result& result)
{
    if (reported)
        return;

    rememberSelection(); // 次に開いたとき、この行を選んで同じ位置に出す（取り消したときも）
    reported = true;
    stopTimer();
    for (auto& scanner : scanners)
        scanner.cancel();

    const auto cb = callback;

    // 「読み込んでいます」を表示してから結果を渡し、渡し終わってから閉じる（安全チェックと読み込みの進み具合は、
    // 結果を受けた側が出すダイアログ（PluginLoadDialog）が出す）
    if (result.kind == Result::Kind::chosen)
    {
        messageLabel.setText (TRANS ("Loading %name%...").replace ("%name%", result.description.name), juce::dontSendNotification);
        for (auto* component : std::initializer_list<juce::Component*> { &searchEditor, &list, &fileButton, &foldersButton, &rescanButton, &cancelButton, &okButton })
            component->setEnabled (false);

        // 読み込みは callAfterDelay の中で直接しない。callAfterDelay はタイマーを止める前に呼び、呼び終わってから自分を消すので、
        // 読み込み中のプラグインが自分でメッセージを回すと（UJAM の UFX-REVERB は初期化の中で回す）同じタイマーがまた発火し、
        // 入れ子で何十個も作ったり（2026-11-06）、内側で消えたタイマーを外側でもう一度消してヒープを壊したりする（同日）。
        // タイマーでは 1 回きりのメッセージ（callAsync）を送るだけにして、タイマーが消えてから読み込む
        juce::Timer::callAfterDelay (150, [cb, result, safe = juce::Component::SafePointer<PluginBrowser> (this)]
        {
            juce::MessageManager::callAsync ([cb, result, safe]
            {
                if (cb != nullptr)
                    cb (result);
                if (safe != nullptr)
                    closeDialogOf (*safe);
            });
        });
        return;
    }

    // 先に画面を閉じてから結果を渡す（結果を受けてファイルを選ぶ画面を出したりするため）
    juce::MessageManager::callAsync ([cb, result] { if (cb != nullptr) cb (result); });
    closeDialogOf (*this);
}

void PluginBrowser::editFolders()
{
    launchDialog (std::make_unique<PluginFolderEditor> ([safe = juce::Component::SafePointer<PluginBrowser> (this)] (bool changed)
                  {
                      // フォルダーを変えたら集め直す（保存してある結果は使う。新しいファイルだけ調べる）
                      if (safe != nullptr && changed && ! safe->reported)
                          safe->buildList (false);
                  }),
                  TRANS ("VST3 folders"), Theme::minWindowWidth, Theme::minWindowHeight); // ほかの画面と同じ最小の大きさ
}

//==============================================================================
int PluginBrowser::getNumRows()
{
    return visible.size();
}

void PluginBrowser::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow (row, visible.size()))
        return;

    const auto& type = items.getReference (visible[row]);

    if (rowIsSelected)
    {
        g.setColour (selectedColour);
        g.fillRect (0, 0, width, height);
    }
    g.setColour (Theme::divider);
    g.fillRect (0, height - 1, width, 1);

    // 左: 名前（上）とメーカー・種類（下）。右: 入っているファイル（WaveShell の版などを見分けるため）
    const int rightWidth = width * 4 / 10;
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (Theme::bodySize, rowIsSelected ? juce::Font::bold : juce::Font::plain));
    g.drawText (type.name, 12, 2, width - rightWidth - 24, 18, juce::Justification::centredLeft, true);

    g.setColour (Theme::subText);
    g.setFont (juce::FontOptions (Theme::smallSize - 1.0f));
    const auto detail = type.category.isNotEmpty() ? type.manufacturerName + "  |  " + type.category : type.manufacturerName;
    g.drawText (detail, 12, 19, width - rightWidth - 24, 15, juce::Justification::centredLeft, true);

    const auto fileText = juce::File (type.fileOrIdentifier).getFileNameWithoutExtension() + (type.version.isNotEmpty() ? "  v" + type.version : juce::String());

    // 起動している間に安全チェックで読み込めないと分かったものは、右の上に印と短い理由を出す（理由の全文はツールチップ。
    // 選べば、もう一度チェックするかを聞く）
    const auto* verdict = PluginSafety::isEnabled() ? PluginSafety::findVerdict (type) : nullptr;
    if (verdict != nullptr && verdict->blocked)
    {
        g.setColour (Theme::error);
        g.setFont (juce::FontOptions (Theme::smallSize, juce::Font::bold));
        g.drawText (TRANS ("Cannot load: %reason%").replace ("%reason%", PluginSafety::describeShort (*verdict)),
                    width - rightWidth, 2, rightWidth - 12, 17, juce::Justification::centredRight, true);
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (Theme::smallSize - 1.0f));
        g.drawText (fileText, width - rightWidth, 19, rightWidth - 12, 15, juce::Justification::centredRight, true);
        return;
    }

    g.setColour (Theme::dimText);
    g.drawText (fileText, width - rightWidth, 0, rightWidth - 12, height, juce::Justification::centredRight, true);
}

juce::String PluginBrowser::getTooltipForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, visible.size()) || ! PluginSafety::isEnabled())
        return {};
    const auto& type = items.getReference (visible[row]);
    if (const auto* verdict = PluginSafety::findVerdict (type); verdict != nullptr && verdict->blocked)
        return PluginSafety::describe (*verdict, type.name);
    return {};
}

void PluginBrowser::listBoxItemDoubleClicked (int, const juce::MouseEvent&)
{
    chooseSelected();
}

void PluginBrowser::returnKeyPressed (int)
{
    chooseSelected();
}

void PluginBrowser::selectedRowsChanged (int lastRowSelected)
{
    okButton.setEnabled (juce::isPositiveAndBelow (lastRowSelected, visible.size()) && ! reported);

    // 自分で別の行を選んだら、前回の行はもう選ばない（調べ終わって後から出てきても）
    if (! restoring)
        restoreIdentifier = {};
}

//==============================================================================
void PluginBrowser::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBackground);
}

void PluginBrowser::resized()
{
    auto area = getLocalBounds().reduced (16, 12);

    // 下: 左に［ファイルから追加...］［VST3 フォルダー...］［一覧を作り直す］、右に［キャンセル］［追加 / 置き換える］
    auto buttons = area.removeFromBottom (Theme::controlHeight);
    okButton.setBounds (buttons.removeFromRight (juce::jmax (90, Theme::buttonWidth (okButton))));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (Theme::buttonWidth (cancelButton)));
    for (auto* button : { &fileButton, &foldersButton, &rescanButton })
    {
        button->setBounds (buttons.removeFromLeft (Theme::buttonWidth (*button)));
        buttons.removeFromLeft (8);
    }
    area.removeFromBottom (10);

    auto searchRow = area.removeFromTop (Theme::controlHeight);
    sortBox.setBounds (searchRow.removeFromRight (juce::jmax (130, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::bodySize),
                                                                                                             TRANS ("Sort by maker")) + 44)));
    searchRow.removeFromRight (8);
    effectsOnlyToggle.setBounds (searchRow.removeFromRight (juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (Theme::bodySize),
                                                                                                    effectsOnlyToggle.getButtonText()) + 34));
    searchRow.removeFromRight (8);
    searchEditor.setBounds (searchRow);
    area.removeFromTop (6);
    messageLabel.setBounds (area.removeFromTop (38));
    area.removeFromTop (4);
    list.setBounds (area);
    applyRestoredScroll(); // 開いた直後は、ここで一覧の大きさが決まってから位置を合わせる
}
