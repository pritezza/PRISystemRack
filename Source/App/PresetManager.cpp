#include "PresetManager.h"

#include "DialogLaunch.h"
#include "ScaledContent.h"
#include "Theme.h"
#include "WindowRendering.h"

namespace
{
constexpr int rowHeight = 32;
constexpr int rowGap = 4;
constexpr int rowPitch = rowHeight + rowGap;
constexpr int gripWidth = 28;
} // namespace

//==============================================================================
// 並びを入れる入れ物。ドラッグ中は入る位置に線を引く
class PresetManager::List final : public juce::Component
{
public:
    explicit List (PresetManager& ownerToUse) : owner (ownerToUse) {}

    void paintOverChildren (juce::Graphics& g) override
    {
        if (owner.draggingRow == nullptr || owner.dropIndex < 0 || owner.dropIndex == owner.dragFromIndex)
            return;

        // 上へ動かすなら入る行の上、下へ動かすなら入る行の下に線を引く
        const int y = owner.dropIndex < owner.dragFromIndex ? owner.dropIndex * rowPitch - rowGap / 2
                                                             : (owner.dropIndex + 1) * rowPitch - rowGap / 2;
        g.setColour (Theme::accent);
        g.fillRect (0, y - 1, getWidth(), 3);
    }

private:
    PresetManager& owner;
};

//==============================================================================
// プリセット 1 つ分の行: つまみ、名前（ダブルクリックで変更）、[削除]
class PresetManager::Row final : public juce::Component
{
public:
    Row (PresetManager& ownerToUse, const ChainPresets::Preset& presetToShow)
        : owner (ownerToUse), preset (presetToShow)
    {
        deleteButton.setButtonText (TRANS ("Delete"));
        Theme::styleButton (deleteButton);
        deleteButton.setColour (juce::TextButton::textColourOffId, Theme::error);
        deleteButton.onClick = [this] { owner.confirmDelete (preset.id, preset.name); };
        addAndMakeVisible (deleteButton);
    }

    const ChainPresets::Preset& getPreset() const { return preset; }

    void setDragging (bool isDragging)
    {
        dragging = isDragging;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (dragging ? Theme::cardRaised : Theme::card);
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (dragging ? Theme::accent : Theme::cardBorder);
        g.drawRoundedRectangle (area, 6.0f, 1.0f);

        // つまみ（点 2 列 × 3 段）
        g.setColour (Theme::dimText);
        const float cx = gripWidth * 0.5f + 4.0f, cy = (float) getHeight() * 0.5f;
        for (int column = 0; column < 2; ++column)
            for (int row = -1; row <= 1; ++row)
                g.fillEllipse (cx - 4.0f + (float) column * 6.0f - 1.5f, cy + (float) row * 6.0f - 1.5f, 3.0f, 3.0f);

        if (editor == nullptr)
        {
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (Theme::bodySize));
            g.drawText (preset.name, nameArea(), juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        deleteButton.setBounds (getLocalBounds().removeFromRight (Theme::buttonWidth (deleteButton) + 8)
                                    .withTrimmedRight (8)
                                    .withSizeKeepingCentre (Theme::buttonWidth (deleteButton), Theme::controlHeight));
        if (editor != nullptr)
            editor->setBounds (nameArea().withSizeKeepingCentre (nameArea().getWidth(), Theme::controlHeight));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (e.x < gripWidth + 4 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        gripPressed = e.x < gripWidth + 4 && editor == nullptr;
        if (gripPressed)
            owner.rowDragStarted (*this);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (gripPressed)
            owner.rowDragged (e.getEventRelativeTo (owner.list.get()));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (gripPressed)
            owner.rowDragEnded();
        gripPressed = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (nameArea().contains (e.getPosition()))
            startRename();
    }

private:
    juce::Rectangle<int> nameArea() const
    {
        return getLocalBounds().withTrimmedLeft (gripWidth + 10).withTrimmedRight (Theme::buttonWidth (deleteButton) + 24);
    }

    void startRename()
    {
        editor = std::make_unique<juce::TextEditor>();
        editor->setFont (juce::FontOptions (Theme::bodySize));
        editor->setText (preset.name, false);
        editor->onReturnKey = [this] { finishRename (true); };
        editor->onEscapeKey = [this] { finishRename (false); };
        editor->onFocusLost = [this] { finishRename (true); };
        addAndMakeVisible (*editor);
        resized();
        editor->selectAll();
        editor->grabKeyboardFocus();
        repaint();
    }

    void finishRename (bool apply)
    {
        if (editor == nullptr)
            return;

        editor->onFocusLost = nullptr;
        const auto newName = editor->getText().trim();

        // 入力欄はこの呼び出しの中から消さない
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Row> (this)]
        {
            if (safe != nullptr)
            {
                safe->editor.reset();
                safe->repaint();
            }
        });

        if (apply && newName.isNotEmpty() && newName != preset.name)
            owner.rename (preset.id, newName);
    }

    PresetManager& owner;
    ChainPresets::Preset preset;
    juce::TextButton deleteButton;
    std::unique_ptr<juce::TextEditor> editor;
    bool dragging = false;
    bool gripPressed = false;
};

//==============================================================================
PresetManager::PresetManager (ChainPresets& presetsToEdit, std::function<void()> onChangedCallback)
    : presets (presetsToEdit), onChanged (std::move (onChangedCallback))
{
    hintLabel.setText (TRANS ("Drag the handle on the left to reorder. Double-click a name to rename it."), juce::dontSendNotification);
    hintLabel.setColour (juce::Label::textColourId, Theme::subText);
    hintLabel.setFont (juce::FontOptions (Theme::smallSize));
    hintLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (hintLabel);

    emptyLabel.setText (TRANS ("There are no presets yet. Save one with Save as new."), juce::dontSendNotification);
    emptyLabel.setColour (juce::Label::textColourId, Theme::dimText);
    emptyLabel.setFont (juce::FontOptions (Theme::bodySize));
    emptyLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (emptyLabel);

    list = std::make_unique<List> (*this);
    viewport.setViewedComponent (list.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    closeButton.setButtonText (TRANS ("Close"));
    Theme::styleButton (closeButton);
    closeButton.onClick = [this]
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0);
    };
    addAndMakeVisible (closeButton);

    rebuild();
    setSize (460, 420);
}

PresetManager::~PresetManager()
{
    rows.clear(); // 行は list の子なので、list より先に消す
}

void PresetManager::show (ChainPresets& presets, std::function<void()> onChanged)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ScaledContent (std::make_unique<PresetManager> (presets, std::move (onChanged)), Theme::effectiveScale()));
    options.dialogTitle = TRANS ("Manage presets");
    options.dialogBackgroundColour = Theme::windowBackground;
    options.useNativeTitleBar = true;
    options.resizable = true;

    if (auto* window = DialogLaunch::launchAsync (options))
        WindowRendering::install (*window);
}

void PresetManager::rebuild()
{
    rows.clear();
    draggingRow = nullptr;

    for (const auto& preset : presets.getPresets())
    {
        auto row = std::make_unique<Row> (*this, preset);
        list->addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    emptyLabel.setVisible (rows.empty());
    resized();
}

void PresetManager::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBackground);
}

void PresetManager::resized()
{
    auto area = getLocalBounds().reduced (14, 12);
    hintLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (6);

    closeButton.setBounds (area.removeFromBottom (Theme::controlHeight).removeFromRight (juce::jmax (80, Theme::buttonWidth (closeButton))));
    area.removeFromBottom (10);

    viewport.setBounds (area);
    emptyLabel.setBounds (area);

    const int width = viewport.getMaximumVisibleWidth();
    list->setSize (width, juce::jmax (0, (int) rows.size() * rowPitch - rowGap));
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].get() != draggingRow)
            rows[i]->setBounds (0, (int) i * rowPitch, width, rowHeight);
}

//==============================================================================
void PresetManager::rowDragStarted (Row& row)
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].get() == &row)
            dragFromIndex = (int) i;

    draggingRow = &row;
    dropIndex = dragFromIndex;
    dragOffsetY = row.getMouseXYRelative().y;
    row.setDragging (true);
    row.toFront (false);
}

void PresetManager::rowDragged (const juce::MouseEvent& eventInList)
{
    if (draggingRow == nullptr)
        return;

    const int top = juce::jlimit (0, juce::jmax (0, list->getHeight() - rowHeight), eventInList.y - dragOffsetY);
    draggingRow->setTopLeftPosition (0, top);
    dropIndex = insertIndexAt (top + rowHeight / 2);
    list->repaint();
}

void PresetManager::rowDragEnded()
{
    if (draggingRow == nullptr)
        return;

    const auto id = draggingRow->getPreset().id;
    const int target = dropIndex;
    draggingRow->setDragging (false);
    draggingRow = nullptr;
    dropIndex = -1;

    // 行を作り直すので、この行のマウスの処理が終わってから行う
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PresetManager> (this), id, target]
    {
        if (safe == nullptr)
            return;
        if (safe->presets.move (id, target) && safe->onChanged != nullptr)
            safe->onChanged();
        safe->rebuild();
    });
    resized();
    list->repaint();
}

int PresetManager::insertIndexAt (int y) const
{
    return juce::jlimit (0, juce::jmax (0, (int) rows.size() - 1), y / rowPitch);
}

void PresetManager::rename (const juce::String& id, const juce::String& newName)
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PresetManager> (this), id, newName]
    {
        if (safe == nullptr)
            return;
        if (safe->presets.rename (id, newName) && safe->onChanged != nullptr)
            safe->onChanged();
        safe->rebuild();
    });
}

void PresetManager::confirmDelete (const juce::String& id, const juce::String& name)
{
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, TRANS ("Delete preset"),
                                        TRANS ("Delete \"%name%\"? The current chains are not changed.").replace ("%name%", name),
                                        TRANS ("Delete"), TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<PresetManager> (this), id] (int result)
                                        {
                                            if (safe == nullptr || result == 0)
                                                return;
                                            if (safe->presets.remove (id) && safe->onChanged != nullptr)
                                                safe->onChanged();
                                            safe->rebuild();
                                        }));
}
