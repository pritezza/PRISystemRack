#pragma once

// JUCE のヘッダーと一緒に使うので windows.h は include しない（HANDLE は void* で持つ）

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "Common/RackIpc.h"

namespace pri::host
{
// 本体アプリから見た、APO との受け渡し口。
// 専用スレッド（MMCSS "Pro Audio"）で APO からの依頼を待ち、Processor に処理させて結果を返す。
// APO の共有メモリがまだなければ、できるまで 0.5 秒ごとに開き直す。
class ApoLink
{
public:
    // 音声スレッドから呼ばれる。Process ではメモリ確保やロックなど時間の読めない処理をしないこと
    class Processor
    {
    public:
        virtual ~Processor() = default;
        // 形式が決まったとき、変わったときに Process より先に呼ばれる
        virtual void Prepare(uint32_t sampleRate, uint32_t channels, uint32_t maxFrames) = 0;
        // input / output はインターリーブ（L, R, L, R, ...）
        virtual void Process(const float* input, float* output, uint32_t frames, uint32_t channels) = 0;
    };

    struct Stats
    {
        bool connected = false;
        bool apoActive = false;        // APO が処理中（デバイスで音が再生されている）
        uint32_t lastConnectError = 0; // 未接続のときの理由（Win32 のエラー番号。2 = APO がまだ共有メモリを作っていない）
        uint32_t sampleRate = 0;
        uint32_t channels = 0;
        uint32_t maxFrames = 0;
        uint32_t timeoutMicros = 0;
        // 本体側
        uint64_t hostBlocks = 0;
        double processMicrosAverage = 0.0;
        double processMicrosMax = 0.0;
        // APO 側（共有メモリの累計）
        uint64_t apoProcessed = 0;
        uint64_t apoTimedOut = 0;
        uint64_t apoNoHost = 0;
        double roundTripMicrosAverage = 0.0;
        double roundTripMicrosMax = 0.0;
    };

    ApoLink(std::wstring endpointGuid, Processor& processor);
    ~ApoLink();

    ApoLink(const ApoLink&) = delete;
    ApoLink& operator=(const ApoLink&) = delete;

    void Start();
    void Stop();

    // 動いている間ならどのスレッドから呼んでもよい（Stop と同時には呼ばないこと）
    Stats GetStats() const;

private:
    void Run();
    bool TryConnect();
    void Disconnect();

    const std::wstring endpointGuid_;
    Processor& processor_;

    std::thread thread_;
    std::atomic<bool> running_{ false };

    void* mapping_ = nullptr;       // HANDLE
    void* requestEvent_ = nullptr;  // HANDLE
    void* responseEvent_ = nullptr; // HANDLE
    std::atomic<ipc::SharedBlock*> block_{ nullptr };

    int64_t qpcFrequency_ = 1;
    std::atomic<uint32_t> lastConnectError_{ 0 };
    std::atomic<uint64_t> hostBlocks_{ 0 };
    std::atomic<int64_t> processTicksSum_{ 0 };
    std::atomic<int64_t> processTicksMax_{ 0 };
};
} // namespace pri::host
