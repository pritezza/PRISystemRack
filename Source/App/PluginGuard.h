#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// プラグインの呼び出しを Windows の構造化例外（アクセス違反など）から守る。
// 例外が起きたら false を返す（そのプラグインはもう使わないこと）。
//
// 守れるのは、この関数の中（呼んだスレッド）で起きた例外だけ。プラグインが自分のスレッドで落ちた場合や、
// メモリーを壊した場合、画面の描画で落ちた場合は防げない。
namespace PluginGuard
{
bool processBlock (juce::AudioPluginInstance* plugin, juce::AudioBuffer<float>* buffer, juce::MidiBuffer* midi) noexcept;
bool prepareToPlay (juce::AudioPluginInstance* plugin, double sampleRate, int blockSize) noexcept;
bool getState (juce::AudioPluginInstance* plugin, juce::MemoryBlock* state) noexcept;
bool setState (juce::AudioPluginInstance* plugin, const void* data, int size) noexcept;
// 解放の途中で落ちたら、残りは手放したままにする（メモリーは漏れる）
void destroy (juce::AudioPluginInstance* plugin) noexcept;
// プラグインの画面を作る（hasEditor と createEditorAndMakeActive）。画面を持たないプラグインは *editor = nullptr で true。
// 画面を作る途中で例外が起きたら false（DodgePro はここで 0 で割って落ちた）
bool createEditor (juce::AudioPluginInstance* plugin, juce::AudioProcessorEditor** editor) noexcept;
} // namespace PluginGuard
