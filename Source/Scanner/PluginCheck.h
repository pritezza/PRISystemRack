#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// VST3 プラグインの読み込み調査（このPCにある VST3 を片っ端から試す）。
//
//   PRISystemRackScanner.exe --check <description.xml> <output.xml>
//     プラグインを 1 つ、本体（PluginChain）と同じ手順で作って準備し、ノイズを約 1 秒通し、設定を保存して戻してもう一度通し、
//     最後に解放する。段階に入るたびに output.xml（<CHECK>）を書き直すので、途中で落ちた・止まったときも、どの段階かが分かる。
//
//   PRISystemRackScanner.exe --check-all <作業フォルダー> <結果の .md>
//     VST3 フォルダー（本体の「VST3 フォルダー」の設定。なければ既定）の .vst3 をすべて、ファイルごとに一覧を読み（自分を
//     別のプロセスで起動）、中のプラグインを 1 つずつ --check で試す。結果は 1 件ごとに作業フォルダーの results.tsv に足し、
//     そのたびに .md を書き直す。途中で止めても（窓を閉じる・再起動）、もう一度起動すれば results.tsv にあるものは飛ばして続きから。
//     ファイルが変わった（大きさか更新日時）ものは試し直す。この処理自体はプラグインを読み込まない。
namespace PluginCheck
{
int checkOne (const juce::File& descriptionFile, const juce::File& outputFile);
int checkAll (const juce::File& workFolder, const juce::File& reportFile, const juce::String& folder = {}); // folder: 試験用（指定すればそこだけ）
} // namespace PluginCheck
