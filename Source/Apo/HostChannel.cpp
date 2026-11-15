#include "HostChannel.h"

#include "ApoLog.h"

#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <new>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace pri::apo
{
namespace
{
// SYSTEM、LOCAL SERVICE（audiodg.exe）、Administrators にはすべて、
// 対話ログオンのユーザー（本体アプリ）には読み書きと待機（GR GW GX）を許可する
constexpr wchar_t kObjectSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGWGX;;;IU)";

// タイマーが働かなかった場合でも、これ以上は待たない
constexpr DWORD kSafetyWaitMs = 20;

void CloseHandleIfValid(HANDLE& handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}
} // namespace

HostChannel::~HostChannel()
{
    Close();
}

bool HostChannel::Open(const wchar_t* endpointGuid, uint64_t ownerToken, uint32_t sampleRate, uint32_t channels,
                       uint32_t maxFrames, uint32_t timeoutMicros)
{
    Close();

    if (endpointGuid == nullptr || endpointGuid[0] == L'\0')
    {
        Log(L"HostChannel: endpoint GUID is unknown");
        return false;
    }
    if (channels == 0 || channels > ipc::kMaxChannels || maxFrames == 0 || maxFrames > ipc::kMaxFrames)
    {
        Log(L"HostChannel: unsupported format channels=%u maxFrames=%u", channels, maxFrames);
        return false;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kObjectSddl, SDDL_REVISION_1, &descriptor, nullptr))
    {
        Log(L"HostChannel: ConvertStringSecurityDescriptor failed error=%lu", GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes = { sizeof(attributes), descriptor, FALSE };

    wchar_t memoryName[ipc::kObjectNameLength];
    wchar_t requestName[ipc::kObjectNameLength];
    wchar_t responseName[ipc::kObjectNameLength];
    ipc::MakeObjectName(memoryName, _countof(memoryName), ipc::ObjectKind::Memory, endpointGuid);
    ipc::MakeObjectName(requestName, _countof(requestName), ipc::ObjectKind::Request, endpointGuid);
    ipc::MakeObjectName(responseName, _countof(responseName), ipc::ObjectKind::Response, endpointGuid);

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(sizeof(ipc::SharedBlock)), memoryName);
    const DWORD mappingError = GetLastError();
    const bool created = mapping_ != nullptr && mappingError != ERROR_ALREADY_EXISTS;

    requestEvent_ = CreateEventW(&attributes, FALSE, FALSE, requestName);
    const DWORD requestError = requestEvent_ != nullptr ? 0 : GetLastError();
    responseEvent_ = CreateEventW(&attributes, FALSE, FALSE, responseName);
    const DWORD responseError = responseEvent_ != nullptr ? 0 : GetLastError();
    LocalFree(descriptor);

    if (mapping_ == nullptr || requestEvent_ == nullptr || responseEvent_ == nullptr)
    {
        Log(L"HostChannel: create failed %ls mapping=%lu request=%lu response=%lu", memoryName,
            mapping_ != nullptr ? 0UL : mappingError, requestError, responseError);
        Close();
        return false;
    }

    void* view = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ipc::SharedBlock));
    if (view == nullptr)
    {
        Log(L"HostChannel: MapViewOfFile failed error=%lu", GetLastError());
        Close();
        return false;
    }

    if (created)
    {
        block_ = new (view) ipc::SharedBlock;
        block_->magic = ipc::kMagic;
        block_->version = ipc::kVersion;
    }
    else
    {
        block_ = static_cast<ipc::SharedBlock*>(view);
        if (block_->magic != ipc::kMagic || block_->version != ipc::kVersion)
        {
            Log(L"HostChannel: existing memory has unexpected magic=0x%08X version=%u", block_->magic, block_->version);
            Close();
            return false;
        }
    }

    // 担当を取る。前の audiodg.exe が落ちて残った担当（別の pid）は引き継ぐ
    uint64_t expected = 0;
    if (block_->apoOwner.compare_exchange_strong(expected, ownerToken))
    {
        isOwner_ = true;
    }
    else if (static_cast<DWORD>(expected >> 32) != GetCurrentProcessId())
    {
        block_->apoOwner.store(ownerToken);
        isOwner_ = true;
        Log(L"HostChannel: took over stale owner 0x%016llX", expected);
    }
    else
    {
        isOwner_ = false;
        Log(L"HostChannel: another instance in this process owns the channel (0x%016llX)", expected);
    }
    ownerToken_ = ownerToken;

    if (isOwner_)
    {
        block_->sampleRate.store(sampleRate);
        block_->channels.store(channels);
        block_->maxFrames.store(maxFrames);
        block_->timeoutMicros.store(timeoutMicros);
        block_->formatSerial.fetch_add(1, std::memory_order_release);
    }

    bool highResolution = true;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer_ == nullptr)
    {
        highResolution = false;
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    timeoutDueTime_.QuadPart = -static_cast<LONGLONG>(timeoutMicros) * 10;
    fallbackWaitMs_ = std::max<DWORD>(1, (timeoutMicros + 999) / 1000);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    heartbeatTimeoutTicks_ = frequency.QuadPart * ipc::kHostHeartbeatTimeoutMs / 1000;

    channels_ = channels;
    maxFrames_ = maxFrames;

    Log(L"HostChannel: opened %ls created=%d owner=%d timeout=%u us timer=%ls", memoryName, created ? 1 : 0,
        isOwner_ ? 1 : 0, timeoutMicros,
        timer_ == nullptr ? L"none" : (highResolution ? L"high-resolution" : L"normal"));
    return true;
}

void HostChannel::Close()
{
    if (block_ != nullptr)
    {
        if (isOwner_)
        {
            uint64_t expected = ownerToken_;
            block_->apoOwner.compare_exchange_strong(expected, 0);
        }
        UnmapViewOfFile(block_);
        block_ = nullptr;
    }
    isOwner_ = false;
    CloseHandleIfValid(timer_);
    CloseHandleIfValid(responseEvent_);
    CloseHandleIfValid(requestEvent_);
    CloseHandleIfValid(mapping_);
}

// リアルタイムスレッドから呼ばれる。メモリ確保やログ出力はしない
HostChannel::Result HostChannel::Process(const float* input, float* output, uint32_t frames)
{
    if (!IsActive() || frames > maxFrames_)
        return Result::Unavailable;

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    const uint32_t hostPid = block_->hostPid.load(std::memory_order_acquire);
    const int64_t heartbeat = block_->hostHeartbeat.load(std::memory_order_acquire);
    if (hostPid == 0 || start.QuadPart - heartbeat > heartbeatTimeoutTicks_)
    {
        block_->blocksNoHost.fetch_add(1, std::memory_order_relaxed);
        return Result::NoHost;
    }

    const size_t samples = static_cast<size_t>(frames) * channels_;
    if (input != nullptr)
        std::memcpy(block_->input, input, samples * sizeof(float));
    else
        std::memset(block_->input, 0, samples * sizeof(float));

    block_->requestFrames.store(frames, std::memory_order_relaxed);
    const uint64_t seq = block_->requestSeq.load(std::memory_order_relaxed) + 1;
    block_->requestSeq.store(seq, std::memory_order_release);

    if (timer_ != nullptr)
        SetWaitableTimer(timer_, &timeoutDueTime_, 0, nullptr, nullptr, FALSE);
    SetEvent(requestEvent_);

    // Response イベントは、前の周期に遅れて戻った結果で立っていることがあるので、番号を確かめる
    const HANDLE handles[2] = { responseEvent_, timer_ };
    const DWORD handleCount = timer_ != nullptr ? 2 : 1;
    const DWORD waitMs = timer_ != nullptr ? kSafetyWaitMs : fallbackWaitMs_;
    bool answered = false;
    for (;;)
    {
        const DWORD result = WaitForMultipleObjects(handleCount, handles, FALSE, waitMs);
        if (result != WAIT_OBJECT_0)
            break; // タイマー、待ち時間切れ、失敗
        if (block_->responseSeq.load(std::memory_order_acquire) == seq)
        {
            answered = true;
            break;
        }
    }
    if (timer_ != nullptr)
        CancelWaitableTimer(timer_);

    if (!answered)
    {
        block_->blocksTimedOut.fetch_add(1, std::memory_order_relaxed);
        return Result::TimedOut;
    }

    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    const int64_t roundTrip = end.QuadPart - start.QuadPart;
    block_->roundTripTicksSum.fetch_add(roundTrip, std::memory_order_relaxed);
    if (roundTrip > block_->roundTripTicksMax.load(std::memory_order_relaxed))
        block_->roundTripTicksMax.store(roundTrip, std::memory_order_relaxed);

    std::memcpy(output, block_->output, samples * sizeof(float));
    block_->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
    return Result::Processed;
}
} // namespace pri::apo
