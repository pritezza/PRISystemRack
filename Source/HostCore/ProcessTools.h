#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// プロセスと音声セッションを調べる（JUCE と一緒に使うので windows.h はここに出さない）
namespace pri::host
{
// exeName（例: L"Gather.exe"）のプロセスツリーの根元の PID をすべて返す（大文字小文字は無視）。
// Electron アプリは同じ名前の子プロセスを多数持つので、親が同じ名前でないものを根元とみなす
std::vector<uint32_t> FindRootProcesses(const std::wstring& exeName);

// roots とその子孫（名前は問わない）の PID
std::set<uint32_t> CollectProcessTree(const std::vector<uint32_t>& roots);

bool IsProcessAlive(uint32_t processId);

// Windows Audio の今の状態（管理者でなくても調べられる）。再起動したかを見分けるのに使う:
// サービス（Audiosrv）の PID か、音声エンジン（audiodg.exe）の PID が変わったら再起動した。メモリーの少ない PC では Audiosrv が
// ほかのサービスと同じ svchost で動き、再起動しても PID が変わらないことがあるので、audiodg.exe（Audiosrv が起動し直す）も見る
struct AudioServiceState
{
    bool known = false;       // 調べられた（false なら、ほかの値は使わない。一時的に失敗しただけで「止まった」とは見なさない）
    uint32_t servicePid = 0;  // 0 = Audiosrv が止まっている
    uint32_t audiodgPid = 0;  // 0 = audiodg.exe がいない（音を出していないときなど。比べない）
};
AudioServiceState GetAudioServiceState();

// 有効な再生デバイスにある音声セッション（システム音と自分自身は除く）
struct AudioSessionInfo
{
    uint32_t processId = 0;
    std::wstring endpointGuid; // 大文字（Endpoints と同じ）
    std::wstring endpointName;
    bool active = false;   // 再生の口が動いている（無音のことがある。Gather などは既定のデバイスに無音で開いたままにする）
    float peak = 0.0f;     // 調べた短い間（約 30 ms）の最大音量（動いているセッションだけ。音量・ミュートをかけたあと）
    bool sounding = false; // 実際に音が出ている（動いていて、peak が -70 dB を超えた）
};
std::vector<AudioSessionInfo> ListAudioSessions();

// 対象アプリを選ぶ画面用: exe 名ごとのまとめ
struct ProcessGroup
{
    std::wstring exeName;
    int processCount = 0;
    bool hasAudioSession = false;
    std::vector<std::wstring> activeEndpoints; // いま音を出しているデバイスの名前
};
std::vector<ProcessGroup> ListProcessGroups(); // 名前順、自分自身は除く
} // namespace pri::host
