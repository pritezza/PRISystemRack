#pragma once

// APO（audiodg.exe）と本体アプリの間で共有するメモリの配置と、名前付きオブジェクトの名前。
// 両方のプロセスが同じ定義を使う。配置を変えたら kVersion を上げること（名前にも入る）。
//
// 1 周期の流れ:
//   APO : input に書く → requestFrames → requestSeq を +1（release）→ Request イベント → 結果を待つ
//   本体: requestSeq を読む（acquire）→ input を処理して output に書く → responseSeq = requestSeq（release）→ Response イベント
//   APO : responseSeq が自分の番号になっていれば output を使う。時間切れなら素通し
//
// 本体は hostPid と hostHeartbeat（QueryPerformanceCounter の値）を書く。
// APO は hostPid が 0 か、心拍が kHostHeartbeatTimeoutMs より古ければ待たずに素通しする。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace pri::ipc
{
// APO の CLSID（Source/Apo/ApoGuids.h と同じ値にすること）。本体がデバイスに APO が入っているかを調べるのに使う
inline constexpr wchar_t kApoClsidString[] = L"{C06E99BA-ABC5-4FB7-9943-0306C073ADA5}";

inline constexpr uint32_t kMagic = 0x52535250; // "PRSR"
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxChannels = 8;
inline constexpr uint32_t kMaxFrames = 4096;
inline constexpr uint32_t kMaxSamples = kMaxChannels * kMaxFrames;

// 本体の心拍がこれより古ければ、本体はいないものとみなす
inline constexpr uint32_t kHostHeartbeatTimeoutMs = 500;
// 本体が Request イベントを待つ時間の上限（= 心拍を書く間隔の上限）
inline constexpr uint32_t kHostWaitMs = 100;

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<int64_t>::is_always_lock_free);

// 音声バッファを 64 バイト境界にそろえるので詰め物が入る（C4324 は意図どおり）
#pragma warning(push)
#pragma warning(disable : 4324)
struct SharedBlock
{
    uint32_t magic;
    uint32_t version;

    // --- APO が書く（LockForProcess のとき）---
    std::atomic<uint64_t> apoOwner;     // 処理を担当している APO（(pid << 32) | インスタンス番号）。0 = なし
    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> channels;
    std::atomic<uint32_t> maxFrames;
    std::atomic<uint32_t> timeoutMicros; // APO が結果を待つ時間の上限
    std::atomic<uint32_t> formatSerial;  // 上の値を書いたあとに +1 する。本体はこれを見て準備し直す

    // --- 本体が書く ---
    std::atomic<uint32_t> hostPid;      // 0 = 本体なし
    std::atomic<int64_t> hostHeartbeat; // QueryPerformanceCounter の値

    // --- 周期ごと ---
    std::atomic<uint64_t> requestSeq;    // APO が入力を書いたら +1
    std::atomic<uint32_t> requestFrames; // 今回のフレーム数
    std::atomic<uint64_t> responseSeq;   // 本体が出力を書いたら requestSeq の値にする

    // --- 統計（APO が書く。累計）---
    std::atomic<uint64_t> blocksProcessed; // 本体の結果を使った
    std::atomic<uint64_t> blocksTimedOut;  // 時間切れで素通しにした
    std::atomic<uint64_t> blocksNoHost;    // 本体がいないので素通しにした
    std::atomic<int64_t> roundTripTicksSum; // 本体に渡してから結果が戻るまで（QPC の単位）
    std::atomic<int64_t> roundTripTicksMax;

    // インターリーブ（L, R, L, R, ...）の 32 bit float
    alignas(64) float input[kMaxSamples];
    alignas(64) float output[kMaxSamples];
};
#pragma warning(pop)

enum class ObjectKind
{
    Memory,
    Request,
    Response,
};

inline constexpr size_t kObjectNameLength = 128;

// 例: Global\PRISystemRack.v1.Memory.{53F1DBA7-751B-4634-B061-C49A99BFFC3E}
// endpointGuid は PKEY_AudioEndpoint_GUID の値を大文字にしたもの（名前は大文字小文字を区別する）
inline void MakeObjectName(wchar_t* buffer, size_t count, ObjectKind kind, const wchar_t* endpointGuid)
{
    const wchar_t* kindName = kind == ObjectKind::Memory    ? L"Memory"
                              : kind == ObjectKind::Request ? L"Request"
                                                            : L"Response";
    swprintf_s(buffer, count, L"Global\\PRISystemRack.v%u.%ls.%ls", kVersion, kindName, endpointGuid);
}
} // namespace pri::ipc
