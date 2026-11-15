#include "Language.h"

#include "BinaryData.h"

namespace Language
{
namespace
{
juce::String currentCode = english;
}

void apply (const juce::String& code)
{
    if (code == japanese)
    {
        currentCode = japanese;
        juce::LocalisedStrings::setCurrentMappings (
            new juce::LocalisedStrings (juce::String::fromUTF8 (BinaryData::ja_txt, BinaryData::ja_txtSize), false));
        // 英語版の Windows には入っていないことがあるので、日本語のときだけ使う
        juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName ("Meiryo UI");
    }
    else
    {
        currentCode = english;
        juce::LocalisedStrings::setCurrentMappings (nullptr);
        juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName ("Segoe UI");
    }
}

juce::String current()
{
    return currentCode;
}
} // namespace Language
