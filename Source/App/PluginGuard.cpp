#include "PluginGuard.h"

#include <excpt.h>

// __try を使う関数の中では、片付け（デストラクター）が必要な C++ のオブジェクトを作らないこと（C2712）。
// 例外が起きても、プラグインの中のフレームは片付けられないまま捨てられる。

namespace PluginGuard
{
bool processBlock (juce::AudioPluginInstance* plugin, juce::AudioBuffer<float>* buffer, juce::MidiBuffer* midi) noexcept
{
    __try
    {
        plugin->processBlock (*buffer, *midi);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool prepareToPlay (juce::AudioPluginInstance* plugin, double sampleRate, int blockSize) noexcept
{
    __try
    {
        plugin->prepareToPlay (sampleRate, blockSize);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool getState (juce::AudioPluginInstance* plugin, juce::MemoryBlock* state) noexcept
{
    __try
    {
        plugin->getStateInformation (*state);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool setState (juce::AudioPluginInstance* plugin, const void* data, int size) noexcept
{
    __try
    {
        plugin->setStateInformation (data, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool createEditor (juce::AudioPluginInstance* plugin, juce::AudioProcessorEditor** editor) noexcept
{
    *editor = nullptr;
    __try
    {
        *editor = plugin->hasEditor() ? plugin->createEditorAndMakeActive() : nullptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void destroy (juce::AudioPluginInstance* plugin) noexcept
{
    __try
    {
        delete plugin;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
} // namespace PluginGuard
