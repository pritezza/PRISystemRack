#include "ProcessPicker.h"

#include "DialogLaunch.h"
#include "ScaledContent.h"
#include "Theme.h"
#include "WindowRendering.h"

namespace
{
const juce::Colour backgroundColour = Theme::windowBackground;
const juce::Colour listColour = Theme::rackBackground;
const juce::Colour selectedColour { 0xff1e4257 };
const juce::Colour accentColour = Theme::accent;
const juce::Colour okColour = Theme::ok;

void styleButton (juce::TextButton& button)
{
    Theme::styleButton (button);
}
} // namespace

ProcessPicker::ProcessPicker (const juce::StringArray& currentTargets, std::function<void (const juce::StringArray&)> onAccepted)
    : selected (currentTargets), accepted (std::move (onAccepted))
{
    hintLabel.setText (TRANS ("Click to select or deselect (multiple selection). The audio of the selected apps and their child processes is captured."),
                       juce::dontSendNotification);
    hintLabel.setColour (juce::Label::textColourId, Theme::subText);
    hintLabel.setFont (juce::FontOptions (Theme::bodySize));
    addAndMakeVisible (hintLabel);

    audioOnlyToggle.setButtonText (TRANS ("Only show processes that have audio"));
    audioOnlyToggle.setToggleState (true, juce::dontSendNotification);
    audioOnlyToggle.onClick = [this] { refresh(); };
    addAndMakeVisible (audioOnlyToggle);

    refreshButton.setButtonText (TRANS ("Refresh list"));
    styleButton (refreshButton);
    refreshButton.onClick = [this] { refresh(); };
    addAndMakeVisible (refreshButton);

    list.setModel (this);
    list.setRowHeight (42);
    list.setColour (juce::ListBox::backgroundColourId, listColour);
    list.setColour (juce::ListBox::outlineColourId, Theme::cardBorder);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    nameLabel.setText (TRANS ("Add by name (apps that are not running can be registered too)"), juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId, Theme::subText);
    nameLabel.setFont (juce::FontOptions (Theme::bodySize));
    addAndMakeVisible (nameLabel);

    nameEditor.setTextToShowWhenEmpty (TRANS ("e.g. Gather.exe"), Theme::dimText);
    nameEditor.setFont (juce::FontOptions (Theme::bodySize));
    nameEditor.onReturnKey = [this] { addByName(); };
    addAndMakeVisible (nameEditor);

    addButton.setButtonText (TRANS ("Add"));
    styleButton (addButton);
    addButton.onClick = [this] { addByName(); };
    addAndMakeVisible (addButton);

    cancelButton.setButtonText (TRANS ("Cancel"));
    styleButton (cancelButton);
    cancelButton.onClick = [this] { close (false); };
    addAndMakeVisible (cancelButton);

    okButton.setButtonText ("OK");
    okButton.setColour (juce::TextButton::buttonColourId, Theme::on);
    okButton.setColour (juce::ComboBox::outlineColourId, accentColour);
    okButton.onClick = [this] { close (true); };
    addAndMakeVisible (okButton);

    refresh();
    setSize (640, 700);
}

void ProcessPicker::show (const juce::String& rackName, const juce::StringArray& currentTargets,
                          std::function<void (const juce::StringArray&)> onAccepted)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ScaledContent (std::make_unique<ProcessPicker> (currentTargets, std::move (onAccepted)), Theme::effectiveScale()));
    options.dialogTitle = TRANS ("Choose target apps (%name%)").replace ("%name%", rackName);
    options.dialogBackgroundColour = backgroundColour;
    options.useNativeTitleBar = true;
    options.resizable = true;

    if (auto* window = DialogLaunch::launchAsync (options))
        WindowRendering::install (*window); // ほかのウィンドウと同じ描画方式にする
}

void ProcessPicker::refresh()
{
    rows.clear();
    const bool audioOnly = audioOnlyToggle.getToggleState();

    for (const auto& group : pri::host::ListProcessGroups())
    {
        const juce::String name (group.exeName.c_str());
        if (audioOnly && ! group.hasAudioSession && ! selected.contains (name, true))
            continue;

        Row row;
        row.exeName = name;
        row.processCount = group.processCount;
        row.running = true;
        for (const auto& endpoint : group.activeEndpoints)
            row.activeEndpoints.add (juce::String (endpoint.c_str()));
        rows.push_back (row);
    }

    // 選んでいるのに起動していないアプリ（名前で登録したもの）も出す
    for (const auto& name : selected)
    {
        const bool listed = std::any_of (rows.begin(), rows.end(), [&name] (const Row& row) { return row.exeName.equalsIgnoreCase (name); });
        if (! listed)
            rows.push_back ({ name, 0, {}, false });
    }

    list.updateContent();
    list.repaint();
}

void ProcessPicker::addByName()
{
    auto name = nameEditor.getText().trim();
    if (name.isEmpty())
        return;
    if (! name.endsWithIgnoreCase (".exe"))
        name << ".exe";

    if (! selected.contains (name, true))
        selected.add (name);
    nameEditor.clear();
    refresh();
}

void ProcessPicker::close (bool isAccepted)
{
    if (isAccepted && accepted != nullptr)
        accepted (selected);

    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (isAccepted ? 1 : 0);
}

//==============================================================================
int ProcessPicker::getNumRows()
{
    return (int) rows.size();
}

void ProcessPicker::paintListBoxItem (int rowIndex, juce::Graphics& g, int width, int height, bool)
{
    if (rowIndex < 0 || rowIndex >= (int) rows.size())
        return;

    const auto& row = rows[(size_t) rowIndex];
    const bool isSelected = selected.contains (row.exeName, true);

    if (isSelected)
    {
        g.setColour (selectedColour);
        g.fillRect (0, 0, width, height);
    }
    g.setColour (Theme::divider);
    g.fillRect (0, height - 1, width, 1);

    // 選択のしるし（チェックボックス）
    const juce::Rectangle<float> box (14.0f, (float) height * 0.5f - 9.0f, 18.0f, 18.0f);
    g.setColour (isSelected ? accentColour : Theme::subText);
    if (isSelected)
    {
        g.fillRoundedRectangle (box, 3.0f);
        juce::Path tick;
        tick.startNewSubPath (box.getX() + 3.5f, box.getCentreY());
        tick.lineTo (box.getX() + 7.0f, box.getBottom() - 4.0f);
        tick.lineTo (box.getRight() - 3.5f, box.getY() + 4.0f);
        g.setColour (Theme::rackBackground);
        g.strokePath (tick, juce::PathStrokeType (2.2f));
    }
    else
    {
        g.drawRoundedRectangle (box, 3.0f, 1.5f);
    }

    g.setColour (row.running ? Theme::text : Theme::dimText);
    g.setFont (juce::FontOptions (Theme::bodySize + 1.0f, isSelected ? juce::Font::bold : juce::Font::plain));
    g.drawText (row.exeName, 44, 0, width / 2 - 44, height, juce::Justification::centredLeft, true);

    g.setFont (juce::FontOptions (Theme::smallSize));
    juce::String detail;
    juce::Colour detailColour = Theme::subText;
    if (! row.running)
    {
        detail = TRANS ("Not running (registered by name)");
    }
    else
    {
        detail = TRANS ("%count% processes").replace ("%count%", juce::String (row.processCount));
        if (! row.activeEndpoints.isEmpty())
        {
            detail << "  |  " << TRANS ("Playing to %device%").replace ("%device%", row.activeEndpoints.joinIntoString (", "));
            detailColour = isSelected ? okColour : detailColour;
        }
    }
    g.setColour (detailColour);
    g.drawText (detail, width / 2, 0, width / 2 - 14, height, juce::Justification::centredRight, true);
}

void ProcessPicker::listBoxItemClicked (int rowIndex, const juce::MouseEvent&)
{
    if (rowIndex < 0 || rowIndex >= (int) rows.size())
        return;

    const auto& name = rows[(size_t) rowIndex].exeName;
    if (selected.contains (name, true))
        selected.removeString (name, true);
    else
        selected.add (name);

    list.repaintRow (rowIndex);
}

//==============================================================================
void ProcessPicker::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
}

void ProcessPicker::resized()
{
    auto area = getLocalBounds().reduced (18, 14);

    hintLabel.setBounds (area.removeFromTop (46));
    area.removeFromTop (6);

    auto options = area.removeFromTop (Theme::controlHeight);
    refreshButton.setBounds (options.removeFromRight (Theme::buttonWidth (refreshButton)));
    audioOnlyToggle.setBounds (options);
    area.removeFromTop (8);

    auto buttons = area.removeFromBottom (Theme::controlHeight);
    okButton.setBounds (buttons.removeFromRight (juce::jmax (80, Theme::buttonWidth (okButton))));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (Theme::buttonWidth (cancelButton)));
    area.removeFromBottom (12);

    auto byName = area.removeFromBottom (Theme::controlHeight);
    addButton.setBounds (byName.removeFromRight (juce::jmax (70, Theme::buttonWidth (addButton))));
    byName.removeFromRight (8);
    nameEditor.setBounds (byName);
    nameLabel.setBounds (area.removeFromBottom (28));
    area.removeFromBottom (4);

    list.setBounds (area);
}
