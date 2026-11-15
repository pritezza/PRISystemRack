// HostSim: 本体アプリの代わりに APO とつながる、テスト用のホスト。
// APO から受け取った音にゲインをかけて返す。--delay-us で処理の重さを真似できる（時間切れの確認用）。
//
// 例: CABLE Input の APO とつながり、-12 dB かけて返す。10 秒で終わる（0 なら Ctrl+C まで）
//     HostSim.exe --endpoint "CABLE Input" --gain -12 --seconds 10

#include <windows.h>

#include "HostCore/ApoLink.h"
#include "HostCore/Endpoints.h"

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
std::atomic<bool> g_stopRequested{ false };

BOOL WINAPI OnConsoleControl(DWORD /*type*/)
{
    g_stopRequested = true;
    return TRUE;
}

struct Options
{
    std::wstring endpointName = L"CABLE Input";
    double gainDb = -12.0;
    uint32_t delayMicros = 0;
    double seconds = 0.0; // 0 = Ctrl+C まで
};

bool ParseArgs(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        const wchar_t* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (value == nullptr)
            return false;

        if (arg == L"--endpoint")
            options.endpointName = value;
        else if (arg == L"--gain")
            options.gainDb = _wtof(value);
        else if (arg == L"--delay-us")
            options.delayMicros = static_cast<uint32_t>(_wtoi(value));
        else if (arg == L"--seconds")
            options.seconds = _wtof(value);
        else
            return false;
        ++i;
    }
    return true;
}

// ゲインをかけるだけの処理。delayMicros があればその時間だけ待つ（重いプラグインの代わり）
class GainProcessor final : public pri::host::ApoLink::Processor
{
public:
    GainProcessor(double gainDb, uint32_t delayMicros)
        : gain_(static_cast<float>(std::pow(10.0, gainDb / 20.0)))
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        delayTicks_ = frequency.QuadPart * delayMicros / 1'000'000;
    }

    void Prepare(uint32_t sampleRate, uint32_t channels, uint32_t maxFrames) override
    {
        sampleRate_ = sampleRate;
        channels_ = channels;
        maxFrames_ = maxFrames;
        prepareCount_.fetch_add(1);
    }

    void Process(const float* input, float* output, uint32_t frames, uint32_t channels) override
    {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);

        const size_t samples = static_cast<size_t>(frames) * channels;
        for (size_t i = 0; i < samples; ++i)
            output[i] = input[i] * gain_;

        if (delayTicks_ > 0)
        {
            LARGE_INTEGER now;
            do
            {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart - start.QuadPart < delayTicks_);
        }
    }

    uint32_t PrepareCount() const { return prepareCount_.load(); }

private:
    const float gain_;
    int64_t delayTicks_ = 0;
    std::atomic<uint32_t> sampleRate_{ 0 };
    std::atomic<uint32_t> channels_{ 0 };
    std::atomic<uint32_t> maxFrames_{ 0 };
    std::atomic<uint32_t> prepareCount_{ 0 };
};

void PrintStats(double elapsedSeconds, const pri::host::ApoLink::Stats& s, uint32_t prepareCount)
{
    if (!s.connected)
    {
        std::wprintf(L"[%5.1fs] APO を待っています（error=%u）\n", elapsedSeconds, s.lastConnectError);
        return;
    }
    std::wprintf(L"[%5.1fs] %uHz %uch 周期=%u | 本体: %llu ブロック 処理 平均 %.1fus 最大 %.1fus 準備 %u 回"
                 L" | APO: 処理 %llu 時間切れ %llu 本体なし %llu 往復 平均 %.0fus 最大 %.0fus（上限 %uus）\n",
                 elapsedSeconds, s.sampleRate, s.channels, s.maxFrames, s.hostBlocks, s.processMicrosAverage,
                 s.processMicrosMax, prepareCount, s.apoProcessed, s.apoTimedOut, s.apoNoHost,
                 s.roundTripMicrosAverage, s.roundTripMicrosMax, s.timeoutMicros);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");

    // 再生デバイスの一覧（今ないものも含む）と、同じ出力のまとまり（deviceKey）、今ないものの移り先を出す
    if (argc == 2 && std::wstring(argv[1]) == L"--list-endpoints")
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return 1;

        for (const auto& endpoint : pri::host::ListRenderEndpoints(true))
        {
            std::wprintf(L"%ls %ls  %ls%ls\n    key: %ls\n", endpoint.active ? L"[active]  " : L"[inactive]", endpoint.guid.c_str(),
                         endpoint.friendlyName.c_str(), endpoint.apoInstalled ? L"  [APO]" : L"", endpoint.deviceKey.c_str());
            if (!endpoint.active)
                if (const auto successor = pri::host::FindActiveSuccessor(endpoint.guid))
                    std::wprintf(L"    -> successor: %ls%ls\n", successor->guid.c_str(), successor->apoInstalled ? L"  [APO]" : L"");
        }
        CoUninitialize();
        return 0;
    }

    Options options;
    if (!ParseArgs(argc, argv, options))
    {
        std::fwprintf(stderr, L"usage: HostSim [--endpoint <name>] [--gain <dB>] [--delay-us <us>] [--seconds <s>]\n");
        return 2;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;

    const auto endpoint = pri::host::FindRenderEndpoint(options.endpointName);
    if (!endpoint)
    {
        std::fwprintf(stderr, L"error: 再生デバイス \"%ls\" が 1 つに定まりません\n", options.endpointName.c_str());
        CoUninitialize();
        return 1;
    }
    std::wprintf(L"endpoint: %ls %ls\n", endpoint->friendlyName.c_str(), endpoint->guid.c_str());
    std::wprintf(L"gain    : %.2f dB, delay %u us\n", options.gainDb, options.delayMicros);

    SetConsoleCtrlHandler(OnConsoleControl, TRUE);

    GainProcessor processor(options.gainDb, options.delayMicros);
    pri::host::ApoLink link(endpoint->guid, processor);
    link.Start();

    const ULONGLONG start = GetTickCount64();
    while (!g_stopRequested)
    {
        Sleep(1000);
        const double elapsed = static_cast<double>(GetTickCount64() - start) / 1000.0;
        PrintStats(elapsed, link.GetStats(), processor.PrepareCount());
        std::fflush(stdout);
        if (options.seconds > 0.0 && elapsed >= options.seconds)
            break;
    }

    link.Stop();
    std::wprintf(L"stopped\n");
    CoUninitialize();
    return 0;
}
