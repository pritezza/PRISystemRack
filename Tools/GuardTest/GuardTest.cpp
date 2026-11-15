// GuardTest: VST3 を本体と同じ守り（PluginGuard）を通して処理させ、プラグインが例外を起こしても
// 処理を続けられるかを確かめる。
//
// 例: GuardTest.exe "build\PRICrashTest_artefacts\RelWithDebInfo\VST3\PRICrashTest.vst3"
// 最後の行に "RESULT caught=<0|1> blocks=<例外までに処理したブロック数> survived=1" を出す。

#include <juce_audio_processors/juce_audio_processors.h>

#include "App/PluginGuard.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    std::setvbuf (stdout, nullptr, _IONBF, 0); // パイプ経由でも途中経過がすぐ見えるように

    if (argc < 2)
    {
        std::printf ("usage: GuardTest <plugin.vst3> [seconds] [--destroy]\n"
                     "       GuardTest <plugin.vst3> --list   (中のプラグインの一覧と、調べるのにかかった時間だけ出す)\n");
        return 2;
    }

    const juce::File file (juce::String::fromUTF8 (argv[1]));
    const bool listOnly = argc >= 3 && juce::String (argv[2]) == "--list";
    const double seconds = argc >= 3 && ! listOnly ? juce::String (argv[2]).getDoubleValue() : 6.0;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    juce::OwnedArray<juce::PluginDescription> types;
    const auto scanStart = juce::Time::getMillisecondCounterHiRes();
    for (auto* format : formats.getFormats())
        format->findAllTypesForFile (types, file.getFullPathName());

    if (listOnly)
    {
        for (auto* type : types)
            std::printf ("  %s | %s | %s\n", type->name.toRawUTF8(), type->manufacturerName.toRawUTF8(), type->category.toRawUTF8());
        std::printf ("RESULT types=%d scanMs=%.0f\n", types.size(), juce::Time::getMillisecondCounterHiRes() - scanStart);
        return types.isEmpty() ? 1 : 0;
    }

    if (types.isEmpty())
    {
        std::printf ("error: cannot load %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 480;

    std::printf ("scanMs : %.0f (types=%d)\n", juce::Time::getMillisecondCounterHiRes() - scanStart, types.size());

    juce::String error;
    const auto createStart = juce::Time::getMillisecondCounterHiRes();
    auto plugin = formats.createPluginInstance (*types[0], sampleRate, blockSize, error);
    if (plugin == nullptr)
    {
        std::printf ("error: %s\n", error.toRawUTF8());
        return 1;
    }

    std::printf ("plugin : %s (createMs %.0f)\n", plugin->getName().toRawUTF8(), juce::Time::getMillisecondCounterHiRes() - createStart);
    std::printf ("programs: %d, current %d \"%s\"\n", plugin->getNumPrograms(), plugin->getCurrentProgram(),
                 plugin->getProgramName (plugin->getCurrentProgram()).toRawUTF8());
    if (! PluginGuard::prepareToPlay (plugin.get(), sampleRate, blockSize))
        std::printf ("prepareToPlay: exception caught\n");

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    const int totalBlocks = (int) (seconds * sampleRate / blockSize);
    int caughtAt = -1;

    for (int block = 0; block < totalBlocks; ++block)
    {
        buffer.clear();
        if (caughtAt < 0 && ! PluginGuard::processBlock (plugin.get(), &buffer, &midi))
        {
            caughtAt = block;
            std::printf ("block %d: exception caught, skipping this plugin from now on\n", block);
        }
    }

    std::printf ("continued to block %d after the exception\n", totalBlocks);

    // 例外を起こしたプラグインは、ホスト側のロックが掛かったまま残っていることがあるので解放しない（--destroy で試す）
    if (caughtAt >= 0 && ! (argc >= 4 && juce::String (argv[3]) == "--destroy"))
    {
        std::printf ("leaving the faulted plugin unreleased\n");
        plugin.release();
    }
    else
    {
        std::printf ("destroying plugin...\n");
        PluginGuard::destroy (plugin.release());
        std::printf ("destroyed\n");
    }
    std::printf ("RESULT caught=%d blocks=%d survived=1\n", caughtAt >= 0 ? 1 : 0, caughtAt);
    return 0;
}
