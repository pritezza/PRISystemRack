#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pri::host
{
// 指定したプロセスとその子プロセスが再生している音だけを WASAPI Process Loopback で取り込む
// （Windows 10 2004 以降）。アプリの音量やミュートをかけたあとの音になる（LoopbackMuteTest で確認）。
class ProcessCapture
{
public:
    // インターリーブの float、フレーム数、チャンネル数。取り込みスレッドから呼ばれる
    using Callback = std::function<void(const float* interleaved, uint32_t frames, uint32_t channels)>;

    enum class State
    {
        stopped,  // 始めていない、または Stop した
        starting, // 取り込みの準備中（Process Loopback の有効化は最大 5 秒かかる）
        running,  // 取り込み中
        failed,   // 準備か取り込みに失敗した（GetLastError に理由）
    };

    ProcessCapture() = default;
    ~ProcessCapture();

    ProcessCapture(const ProcessCapture&) = delete;
    ProcessCapture& operator=(const ProcessCapture&) = delete;

    // 取り込みを始める。準備は取り込みスレッドで行うので、すぐに戻る（結果は GetState で見る）
    void Start(uint32_t processId, uint32_t sampleRate, uint32_t channels, Callback callback);
    void Stop(); // 準備中でもすぐに止まる

    State GetState() const { return state_.load(); }
    std::wstring GetLastError() const;

private:
    void Run();
    void SetError(const wchar_t* message, long hr);

    uint32_t processId_ = 0;
    uint32_t sampleRate_ = 0;
    uint32_t channels_ = 0;
    Callback callback_;

    std::thread thread_;
    std::atomic<bool> stopRequested_{ false };
    std::atomic<State> state_{ State::stopped };

    mutable std::mutex errorLock_;
    std::wstring lastError_;
};

// 指定した再生デバイスに無音を流し続ける。ほかに何も鳴っていなくてもオーディオエンジン（と APO）を動かすため
class SilentStream
{
public:
    SilentStream() = default;
    ~SilentStream();

    SilentStream(const SilentStream&) = delete;
    SilentStream& operator=(const SilentStream&) = delete;

    void Start(const std::wstring& endpointGuid); // 同じデバイスで動いていれば何もしない
    void Stop();
    const std::wstring& GetEndpointGuid() const { return endpointGuid_; }

private:
    void Run();

    std::wstring endpointGuid_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{ false };
};
} // namespace pri::host
