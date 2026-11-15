#pragma once

#include <cstdint>
#include <string>

// VST3 プラグインのファイルを読み込まずに（実行せずに）調べ、この PC で読み込めない理由が分かれば返す。
// プラグインの一覧を作るときに、補助プログラムに読み込ませる前に使う（落ちるのを待たずに理由を出すため）。
// JUCE と一緒に使うので windows.h はここに出さない
namespace pri::host
{
struct PluginBinaryProblem
{
    enum class Kind
    {
        none,           // 見つからなかった（読み込めるとは限らない）
        notX64,         // 64 ビット（x64）用ではない。detail = 対象の CPU（"x86" など）
        runtimeMissing, // Visual C++ ランタイム（MSVCP140.dll など）を使うのに、この PC に入っていない。detail = その DLL
        runtimeTooOld,  // この PC の MSVCP140.dll が、プラグインをビルドしたツールより古い。detail = 必要な版、detail2 = この PC の版
        missingDll,     // （以前の版の記録だけ。今は CheckPluginBinary は返さない。FindUnresolvedImports を使う）
    };

    Kind kind = Kind::none;
    std::wstring detail, detail2;
};

// vst3Path はファイル形式（.vst3 が DLL そのもの）でもフォルダー形式（Contents\x86_64-win\<名前>.vst3）でもよい。
// 確かに読み込めないと言えるものだけを返す（それ以外は補助プログラムに実際に読み込ませて決める）
PluginBinaryProblem CheckPluginBinary(const std::wstring& vst3Path);

// 読み込むのに要る DLL のうち、Windows が探す場所に見つからないもの（", " でつなぐ。なければ空）。補助プログラムが読み込みに
// 失敗したとき（エラー 126）に、原因の候補として添える（WinSxS から読み込まれるものなどは確かめられないので、候補にとどめる）
std::wstring FindUnresolvedImports(const std::wstring& vst3Path);

// フォルダー形式なら中の DLL のパス（x64 用がなければ空）、ファイル形式ならそのまま
std::wstring ResolvePluginBinary(const std::wstring& vst3Path);

// Windows のエラー番号・例外のコードの説明（Windows の表示言語で。分からなければ空）
std::wstring DescribeWindowsError(uint32_t code);
} // namespace pri::host
