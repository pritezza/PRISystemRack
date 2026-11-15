#pragma once

#include <windows.h>

#include <cstdint>

#include "Common/RackIpc.h"

namespace pri::apo
{
// APO から見た、本体アプリとの受け渡し口。
// Open/Close は LockForProcess/UnlockForProcess（非リアルタイム）から、Process は APOProcess から呼ぶ。
class HostChannel
{
public:
    enum class Result
    {
        Unavailable, // 開けていない、または別のインスタンスが担当している
        NoHost,      // 本体がいない（待たずに戻った）
        Processed,   // 本体の結果を output に書いた
        TimedOut,    // 時間内に結果が戻らなかった
    };

    HostChannel() = default;
    ~HostChannel();

    HostChannel(const HostChannel&) = delete;
    HostChannel& operator=(const HostChannel&) = delete;

    // 共有メモリとイベントを作る（すでにあれば開く）。失敗の理由はログに出す
    bool Open(const wchar_t* endpointGuid, uint64_t ownerToken, uint32_t sampleRate, uint32_t channels,
              uint32_t maxFrames, uint32_t timeoutMicros);
    void Close();

    // 自分が処理の担当として開けているか
    bool IsActive() const { return block_ != nullptr && isOwner_; }

    // input が nullptr なら無音を渡す。Processed のときだけ output に結果が入る
    Result Process(const float* input, float* output, uint32_t frames);

    // ログ用（非リアルタイム）
    const ipc::SharedBlock* Block() const { return block_; }

private:
    HANDLE mapping_ = nullptr;
    HANDLE requestEvent_ = nullptr;
    HANDLE responseEvent_ = nullptr;
    HANDLE timer_ = nullptr; // 待ち時間の上限を測る（高精度タイマー）
    ipc::SharedBlock* block_ = nullptr;

    uint64_t ownerToken_ = 0;
    bool isOwner_ = false;
    uint32_t channels_ = 0;
    uint32_t maxFrames_ = 0;
    int64_t heartbeatTimeoutTicks_ = 0;
    LARGE_INTEGER timeoutDueTime_ = {}; // SetWaitableTimer 用。負の値 = 相対時間（100 ns 単位）
    DWORD fallbackWaitMs_ = 1;          // タイマーを作れなかったときの待ち時間
};
} // namespace pri::apo
