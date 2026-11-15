#include "ChainPresets.h"

ChainPresets::ChainPresets()
{
    root = juce::parseXMLIfTagMatches (getFile(), "CHAINPRESETS");
    if (root == nullptr)
    {
        root = std::make_unique<juce::XmlElement> ("CHAINPRESETS");
        root->setAttribute ("version", 1);
    }
}

juce::File ChainPresets::getFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("PRISystemRack")
        .getChildFile ("ChainPresets.xml");
}

juce::Array<ChainPresets::Preset> ChainPresets::getPresets() const
{
    juce::Array<Preset> presets;
    for (auto* preset : root->getChildWithTagNameIterator ("PRESET"))
        presets.add ({ preset->getStringAttribute ("id"), preset->getStringAttribute ("name") });
    return presets;
}

juce::String ChainPresets::getName (const juce::String& id) const
{
    if (const auto* preset = find (id))
        return preset->getStringAttribute ("name");
    return {};
}

std::unique_ptr<juce::XmlElement> ChainPresets::getState (const juce::String& id) const
{
    if (const auto* preset = find (id))
        if (const auto* state = preset->getFirstChildElement())
            return std::make_unique<juce::XmlElement> (*state);
    return nullptr;
}

juce::String ChainPresets::add (const juce::String& name, const juce::XmlElement& state)
{
    const auto id = juce::Uuid().toDashedString();
    auto* preset = root->createNewChildElement ("PRESET");
    preset->setAttribute ("id", id);
    preset->setAttribute ("name", makeUniqueName (name));
    preset->setAttribute ("modified", juce::Time::getCurrentTime().toISO8601 (true));
    preset->addChildElement (new juce::XmlElement (state));
    save();
    return id;
}

bool ChainPresets::overwrite (const juce::String& id, const juce::XmlElement& state)
{
    auto* preset = find (id);
    if (preset == nullptr)
        return false;

    preset->deleteAllChildElements();
    preset->addChildElement (new juce::XmlElement (state));
    preset->setAttribute ("modified", juce::Time::getCurrentTime().toISO8601 (true));
    save();
    return true;
}

juce::String ChainPresets::duplicate (const juce::String& id, const juce::String& name)
{
    auto* original = find (id);
    if (original == nullptr || name.trim().isEmpty())
        return {};

    auto* copy = new juce::XmlElement (*original);
    const auto newId = juce::Uuid().toDashedString();
    copy->setAttribute ("id", newId);
    copy->setAttribute ("name", makeUniqueName (name));
    copy->setAttribute ("modified", juce::Time::getCurrentTime().toISO8601 (true));
    int index = 0; // PRESET 以外の子は置かないので、子の位置 = プリセットの位置
    while (root->getChildElement (index) != original)
        ++index;
    root->insertChildElement (copy, index + 1);
    save();
    return newId;
}

bool ChainPresets::rename (const juce::String& id, const juce::String& newName)
{
    auto* preset = find (id);
    if (preset == nullptr || newName.trim().isEmpty())
        return false;

    preset->setAttribute ("name", makeUniqueName (newName.trim(), id));
    save();
    return true;
}

bool ChainPresets::remove (const juce::String& id)
{
    auto* preset = find (id);
    if (preset == nullptr)
        return false;

    root->removeChildElement (preset, true);
    save();
    return true;
}

bool ChainPresets::move (const juce::String& id, int newIndex)
{
    auto* preset = find (id);
    if (preset == nullptr)
        return false;

    // PRESET 以外の子は置かないので、子の位置 = プリセットの位置
    int from = 0;
    while (root->getChildElement (from) != preset)
        ++from;
    newIndex = juce::jlimit (0, root->getNumChildElements() - 1, newIndex);
    if (from == newIndex)
        return false;

    root->removeChildElement (preset, false);
    root->insertChildElement (preset, newIndex);
    save();
    return true;
}

juce::String ChainPresets::makeUniqueName (const juce::String& name, const juce::String& ignoreId) const
{
    auto taken = [this, &ignoreId] (const juce::String& candidate)
    {
        for (auto* preset : root->getChildWithTagNameIterator ("PRESET"))
            if (preset->getStringAttribute ("id") != ignoreId && preset->getStringAttribute ("name") == candidate)
                return true;
        return false;
    };

    const auto base = name.trim();
    if (! taken (base))
        return base;

    for (int number = 2;; ++number)
        if (const auto candidate = base + " (" + juce::String (number) + ")"; ! taken (candidate))
            return candidate;
}

juce::XmlElement* ChainPresets::find (const juce::String& id) const
{
    if (id.isEmpty())
        return nullptr;

    for (auto* preset : root->getChildWithTagNameIterator ("PRESET"))
        if (preset->getStringAttribute ("id") == id)
            return preset;
    return nullptr;
}

void ChainPresets::save()
{
    const auto file = getFile();
    file.getParentDirectory().createDirectory();
    root->writeTo (file); // 一時ファイルに書いてから置き換えるので、途中で落ちても前の内容は残る
}
