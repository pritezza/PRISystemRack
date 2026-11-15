#include "ApoLink.h"

#include <windows.h>

#include <avrt.h>

#include <utility>

namespace pri::host
{
namespace
{
void CloseHandleIfValid(HANDLE& handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void UpdateMax(std::atomic<int64_t>& target, int64_t value)
{
    if (value > target.load(std::memory_order_relaxed))
        target.store(value, std::memory_order_relaxed);
}
} // namespace

ApoLink::ApoLink(std::wstring endpointGuid, Processor& processor)
    : endpointGuid_(std::move(endpointGuid)),
      processor_(processor)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    qpcFrequency_ = frequency.QuadPart;
}

ApoLink::~ApoLink()
{
    Stop();
}

void ApoLink::Start()
{
    if (running_.exchange(true))
        return;
    thread_ = std::thread([this] { Run(); });
}

void ApoLink::Stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

ApoLink::Stats ApoLink::GetStats() const
{
    Stats stats;
    const double toMicros = 1'000'000.0 / static_cast<double>(qpcFrequency_);

    stats.lastConnectError = lastConnectError_.load();
    stats.hostBlocks = hostBlocks_.load();
    if (stats.hostBlocks > 0)
        stats.processMicrosAverage = static_cast<double>(processTicksSum_.load()) * toMicros / static_cast<double>(stats.hostBlocks);
    stats.processMicrosMax = static_cast<double>(processTicksMax_.load()) * toMicros;

    const ipc::SharedBlock* block = block_.load(std::memory_order_acquire);
    if (block == nullptr)
        return stats;

    stats.connected = true;
    stats.apoActive = block->apoOwner.load() != 0;
    stats.sampleRate = block->sampleRate.load();
    stats.channels = block->channels.load();
    stats.maxFrames = block->maxFrames.load();
    stats.timeoutMicros = block->timeoutMicros.load();
    stats.apoProcessed = block->blocksProcessed.load();
    stats.apoTimedOut = block->blocksTimedOut.load();
    stats.apoNoHost = block->blocksNoHost.load();
    if (stats.apoProcessed > 0)
        stats.roundTripMicrosAverage = static_cast<double>(block->roundTripTicksSum.load()) * toMicros / static_cast<double>(stats.apoProcessed);
    stats.roundTripMicrosMax = static_cast<double>(block->roundTripTicksMax.load()) * toMicros;
    return stats;
}

void ApoLink::Run()
{
    DWORD taskIndex = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss != nullptr)
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    uint64_t lastSeq = 0;
    uint32_t preparedSerial = 0;
    bool prepared = false;

    while (running_.load(std::memory_order_relaxed))
    {
        ipc::SharedBlock* block = block_.load(std::memory_order_relaxed);
        if (block == nullptr)
        {
            if (!TryConnect())
            {
                Sleep(500);
                continue;
            }
            block = block_.load(std::memory_order_relaxed);
            lastSeq = block->requestSeq.load(std::memory_order_acquire); // つながる前の依頼は無視する
            prepared = false;
        }

        // 心拍。APO はこれが古くなると本体はいないとみなして待たなくなる
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        block->hostHeartbeat.store(now.QuadPart, std::memory_order_release);

        if (WaitForSingleObject(requestEvent_, ipc::kHostWaitMs) != WAIT_OBJECT_0)
            continue;

        // 遅れていた場合は最新の依頼だけ処理する（古い依頼の結果は APO が使わない）
        const uint64_t seq = block->requestSeq.load(std::memory_order_acquire);
        if (seq == lastSeq)
            continue;
        lastSeq = seq;

        const uint32_t serial = block->formatSerial.load(std::memory_order_acquire);
        if (!prepared || serial != preparedSerial)
        {
            processor_.Prepare(block->sampleRate.load(), block->channels.load(), block->maxFrames.load());
            preparedSerial = serial;
            prepared = true;
        }

        const uint32_t channels = block->channels.load(std::memory_order_relaxed);
        const uint32_t frames = block->requestFrames.load(std::memory_order_relaxed);
        if (channels == 0 || channels > ipc::kMaxChannels || frames > ipc::kMaxFrames)
            continue; // 答えなければ APO は時間切れで素通しにする

        LARGE_INTEGER processStart;
        QueryPerformanceCounter(&processStart);
        processor_.Process(block->input, block->output, frames, channels);
        LARGE_INTEGER processEnd;
        QueryPerformanceCounter(&processEnd);

        block->responseSeq.store(seq, std::memory_order_release);
        SetEvent(responseEvent_);

        const int64_t ticks = processEnd.QuadPart - processStart.QuadPart;
        hostBlocks_.fetch_add(1, std::memory_order_relaxed);
        processTicksSum_.fetch_add(ticks, std::memory_order_relaxed);
        UpdateMax(processTicksMax_, ticks);
    }

    Disconnect();
    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics(mmcss);
}

bool ApoLink::TryConnect()
{
    wchar_t memoryName[ipc::kObjectNameLength];
    wchar_t requestName[ipc::kObjectNameLength];
    wchar_t responseName[ipc::kObjectNameLength];
    ipc::MakeObjectName(memoryName, _countof(memoryName), ipc::ObjectKind::Memory, endpointGuid_.c_str());
    ipc::MakeObjectName(requestName, _countof(requestName), ipc::ObjectKind::Request, endpointGuid_.c_str());
    ipc::MakeObjectName(responseName, _countof(responseName), ipc::ObjectKind::Response, endpointGuid_.c_str());

    auto fail = [this](DWORD error) {
        lastConnectError_ = error;
        CloseHandleIfValid(responseEvent_);
        CloseHandleIfValid(requestEvent_);
        CloseHandleIfValid(mapping_);
        return false;
    };

    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, memoryName);
    if (mapping_ == nullptr)
        return fail(GetLastError());

    void* view = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ipc::SharedBlock));
    if (view == nullptr)
        return fail(GetLastError());
    auto* block = static_cast<ipc::SharedBlock*>(view);
    if (block->magic != ipc::kMagic || block->version != ipc::kVersion)
    {
        UnmapViewOfFile(view);
        return fail(ERROR_REVISION_MISMATCH);
    }

    requestEvent_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, requestName);
    responseEvent_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, responseName);
    if (requestEvent_ == nullptr || responseEvent_ == nullptr)
    {
        const DWORD error = GetLastError();
        UnmapViewOfFile(view);
        return fail(error);
    }

    // 別の本体が動いていれば譲る（心拍が古ければ、落ちたものとみなして引き継ぐ）
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const DWORD pid = GetCurrentProcessId();
    const uint32_t currentHost = block->hostPid.load(std::memory_order_acquire);
    const int64_t heartbeatTimeoutTicks = qpcFrequency_ * ipc::kHostHeartbeatTimeoutMs / 1000;
    if (currentHost != 0 && currentHost != pid
        && now.QuadPart - block->hostHeartbeat.load(std::memory_order_acquire) < heartbeatTimeoutTicks)
    {
        UnmapViewOfFile(view);
        return fail(ERROR_BUSY);
    }

    block->hostHeartbeat.store(now.QuadPart, std::memory_order_release);
    block->hostPid.store(pid, std::memory_order_release);
    lastConnectError_ = 0;
    block_.store(block, std::memory_order_release);
    return true;
}

void ApoLink::Disconnect()
{
    ipc::SharedBlock* block = block_.exchange(nullptr);
    if (block != nullptr)
    {
        // 本体がいなくなったことを APO にすぐ知らせる（以後 APO は待たずに素通しする）
        uint32_t pid = GetCurrentProcessId();
        block->hostPid.compare_exchange_strong(pid, 0);
        UnmapViewOfFile(block);
    }
    CloseHandleIfValid(responseEvent_);
    CloseHandleIfValid(requestEvent_);
    CloseHandleIfValid(mapping_);
}
} // namespace pri::host
