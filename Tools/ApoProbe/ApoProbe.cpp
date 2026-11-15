// ApoProbe: 再生デバイスに信号を流し、録音デバイスに届いた音を測る確認用ツール。
//
// レベル（既定）: サイン波を流し、送った音との差を測る。最後の行に "RESULT gain_db=<値>"
//     ApoProbe.exe --render "CABLE Input" --capture "CABLE Output"
// 遅延（--latency）: パルスを流し、再生と録音のタイムスタンプから遅延を測る。最後の行に "LATENCY mean_ms=<値> ..."
//     再生側の時刻は IAudioClock の位置、録音側は IAudioCaptureClient のタイムスタンプ（どちらも QPC 基準）から求める。
//     仮想ケーブルの中の遅延も含むので、値そのものより、条件を変えたときの差を見る。

#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr double kPi = 3.14159265358979323846;

struct Options
{
    std::wstring renderName = L"CABLE Input";
    std::wstring captureName = L"CABLE Output";
    double seconds = 3.0;      // 測定時間
    double frequency = 1000.0; // サイン波の周波数
    double levelDb = -12.0;    // サイン波のピーク (dBFS)
    double skipSeconds = 1.0;  // レベルの解析から外す先頭部分
    bool latency = false;      // 遅延を測る
};

void PrintUsage()
{
    std::fwprintf(stderr,
                  L"usage: ApoProbe [--render <name>] [--capture <name>] [--seconds <s>] [--freq <Hz>] [--level <dBFS>] [--latency]\n"
                  L"  <name> はデバイス名の先頭部分（例: \"CABLE Input\"）\n");
}

bool ParseArgs(int argc, wchar_t** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--latency")
        {
            options.latency = true;
            continue;
        }

        const wchar_t* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (arg == L"--help" || arg == L"-h" || value == nullptr)
            return false;

        if (arg == L"--render")
            options.renderName = value;
        else if (arg == L"--capture")
            options.captureName = value;
        else if (arg == L"--seconds")
            options.seconds = _wtof(value);
        else if (arg == L"--freq")
            options.frequency = _wtof(value);
        else if (arg == L"--level")
            options.levelDb = _wtof(value);
        else
            return false;
        ++i;
    }
    return options.seconds > options.skipSeconds + 0.5;
}

std::wstring FriendlyName(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
        name = value.pwszVal;
    PropVariantClear(&value);
    return name;
}

// 有効なデバイスのうち、名前が prefix で始まるものを探す
ComPtr<IMMDevice> FindDevice(IMMDeviceEnumerator* enumerator, EDataFlow flow, const std::wstring& prefix)
{
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)))
        return nullptr;

    UINT count = 0;
    collection->GetCount(&count);

    ComPtr<IMMDevice> found;
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)))
            continue;

        const std::wstring name = FriendlyName(device.Get());
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;

        if (found)
        {
            std::fwprintf(stderr, L"error: \"%ls\" に一致するデバイスが複数あります\n", prefix.c_str());
            return nullptr;
        }
        found = device;
    }
    return found;
}

bool IsFloat32(const WAVEFORMATEX* format)
{
    if (format->wBitsPerSample != 32)
        return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return false;
}

struct CoTaskMemDeleter
{
    void operator()(void* p) const { CoTaskMemFree(p); }
};
using MixFormat = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

HRESULT g_lastError = S_OK; // 直近の失敗。デバイスが無効になったときに再試行するかの判定に使う

int Fail(const wchar_t* what, HRESULT hr)
{
    g_lastError = hr;
    std::fwprintf(stderr, L"error: %ls (hr=0x%08X)\n", what, static_cast<unsigned>(hr));
    return 1;
}

double Median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}

// 録音したパケットの先頭フレームが、録音ストリームのどの位置か
struct CapturePacket
{
    size_t firstFrame;       // captured の中の位置（フレーム）
    UINT64 devicePosition;   // 録音ストリームの位置（フレーム）
};

int AnalyzeLevel(const Options& options, const std::vector<float>& captured, UINT32 channels, UINT32 sampleRate)
{
    const size_t totalFrames = captured.size() / channels;
    const auto skipFrames = static_cast<size_t>(options.skipSeconds * sampleRate);
    if (totalFrames < skipFrames + sampleRate / 2)
    {
        std::fwprintf(stderr, L"error: 録音できた量が足りません（%zu フレーム）\n", totalFrames);
        return 1;
    }

    // サイン波の RMS はピークより 3.01 dB 低い
    const double sentRmsDb = options.levelDb - 20.0 * std::log10(std::sqrt(2.0));
    const size_t frames = totalFrames - skipFrames;
    double gainSum = 0.0;
    for (UINT32 ch = 0; ch < channels; ++ch)
    {
        double sumSquares = 0.0;
        for (size_t frame = skipFrames; frame < totalFrames; ++frame)
        {
            const double sample = captured[frame * channels + ch];
            sumSquares += sample * sample;
        }
        const double rms = std::sqrt(sumSquares / static_cast<double>(frames));
        const double rmsDb = 20.0 * std::log10(rms > 1e-10 ? rms : 1e-10);
        std::wprintf(L"ch%u    : %.2f dBFS RMS (送った音との差 %+.2f dB)\n", ch, rmsDb, rmsDb - sentRmsDb);
        gainSum += rmsDb - sentRmsDb;
    }

    std::wprintf(L"RESULT gain_db=%.2f\n", gainSum / channels);
    return 0;
}

struct LatencyInput
{
    const std::vector<float>& captured;
    UINT32 captureChannels;
    UINT32 captureRate;
    const std::vector<CapturePacket>& packets;
    double captureOrigin; // 録音ストリームの位置 0 の時刻（秒、QPC 基準）
    UINT32 renderRate;
    double renderOrigin;  // 再生ストリームの位置 0 が鳴った時刻（秒、QPC 基準）
    UINT64 firstPulse;    // 最初のパルスの再生位置（フレーム）
    UINT64 pulseInterval; // パルスの間隔（フレーム）
    UINT64 renderedFrames;
};

int AnalyzeLatency(const LatencyInput& in)
{
    const size_t totalFrames = in.captured.size() / in.captureChannels;
    const auto refractory = static_cast<size_t>(in.captureRate / 10); // 検出したら 100 ms は見ない
    const double pulseIntervalSeconds = static_cast<double>(in.pulseInterval) / in.renderRate;
    constexpr float kThreshold = 0.05f; // パルスは 0.5。-12 dB で 0.125 になる

    std::vector<double> latencies;
    size_t packetIndex = 0;
    for (size_t frame = 0; frame < totalFrames; ++frame)
    {
        if (std::fabs(in.captured[frame * in.captureChannels]) < kThreshold)
            continue;

        while (packetIndex + 1 < in.packets.size() && in.packets[packetIndex + 1].firstFrame <= frame)
            ++packetIndex;
        const CapturePacket& packet = in.packets[packetIndex];
        const double devicePosition = static_cast<double>(packet.devicePosition + (frame - packet.firstFrame));
        const double captureTime = in.captureOrigin + devicePosition / in.captureRate;

        // 録音した時刻の直前に鳴ったパルスを探す
        const double renderPosition = (captureTime - in.renderOrigin) * in.renderRate;
        if (renderPosition >= static_cast<double>(in.firstPulse))
        {
            const auto index = static_cast<UINT64>((renderPosition - static_cast<double>(in.firstPulse)) / static_cast<double>(in.pulseInterval));
            const UINT64 pulseFrame = in.firstPulse + index * in.pulseInterval;
            const double latency = captureTime - (in.renderOrigin + static_cast<double>(pulseFrame) / in.renderRate);
            if (pulseFrame < in.renderedFrames && latency >= 0.0 && latency < pulseIntervalSeconds)
                latencies.push_back(latency * 1000.0);
        }
        frame += refractory;
    }

    // 最初の 2 つはストリーム開始直後なので外す
    if (latencies.size() > 4)
        latencies.erase(latencies.begin(), latencies.begin() + 2);
    if (latencies.empty())
    {
        std::fwprintf(stderr, L"error: パルスを検出できませんでした\n");
        return 1;
    }

    double sum = 0.0;
    for (const double value : latencies)
        sum += value;
    const auto [minIt, maxIt] = std::minmax_element(latencies.begin(), latencies.end());
    std::wprintf(L"LATENCY mean_ms=%.2f min_ms=%.2f max_ms=%.2f pulses=%zu\n", sum / latencies.size(), *minIt,
                 *maxIt, latencies.size());
    return 0;
}

int Run(const Options& options)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
        return Fail(L"MMDeviceEnumerator を作れません", hr);

    const ComPtr<IMMDevice> renderDevice = FindDevice(enumerator.Get(), eRender, options.renderName);
    const ComPtr<IMMDevice> captureDevice = FindDevice(enumerator.Get(), eCapture, options.captureName);
    if (!renderDevice || !captureDevice)
    {
        std::fwprintf(stderr, L"error: デバイスが見つかりません（render=\"%ls\" capture=\"%ls\"）\n",
                      options.renderName.c_str(), options.captureName.c_str());
        return 1;
    }
    std::wprintf(L"render : %ls\n", FriendlyName(renderDevice.Get()).c_str());
    std::wprintf(L"capture: %ls\n", FriendlyName(captureDevice.Get()).c_str());

    ComPtr<IAudioClient> renderClient;
    ComPtr<IAudioClient> captureClient;
    hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(renderClient.GetAddressOf()));
    if (FAILED(hr))
        return Fail(L"再生デバイスを開けません", hr);
    hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(captureClient.GetAddressOf()));
    if (FAILED(hr))
        return Fail(L"録音デバイスを開けません", hr);

    WAVEFORMATEX* rawFormat = nullptr;
    renderClient->GetMixFormat(&rawFormat);
    const MixFormat renderFormat(rawFormat);
    rawFormat = nullptr;
    captureClient->GetMixFormat(&rawFormat);
    const MixFormat captureFormat(rawFormat);
    if (!renderFormat || !captureFormat || !IsFloat32(renderFormat.get()) || !IsFloat32(captureFormat.get()))
    {
        std::fwprintf(stderr, L"error: 共有モードの形式が 32 bit float ではありません\n");
        return 1;
    }
    std::wprintf(L"format : render %lu Hz %u ch / capture %lu Hz %u ch\n",
                 renderFormat->nSamplesPerSec, renderFormat->nChannels,
                 captureFormat->nSamplesPerSec, captureFormat->nChannels);

    constexpr REFERENCE_TIME kBufferDuration = 1'000'000; // 100 ms
    hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, renderFormat.get(), nullptr);
    if (FAILED(hr))
        return Fail(L"再生ストリームを初期化できません", hr);
    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration * 2, 0, captureFormat.get(), nullptr);
    if (FAILED(hr))
        return Fail(L"録音ストリームを初期化できません", hr);

    UINT32 renderBufferFrames = 0;
    renderClient->GetBufferSize(&renderBufferFrames);

    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioClock> clock;
    if (FAILED(hr = renderClient->GetService(IID_PPV_ARGS(&render))))
        return Fail(L"IAudioRenderClient を取得できません", hr);
    if (FAILED(hr = captureClient->GetService(IID_PPV_ARGS(&capture))))
        return Fail(L"IAudioCaptureClient を取得できません", hr);
    if (FAILED(hr = renderClient->GetService(IID_PPV_ARGS(&clock))))
        return Fail(L"IAudioClock を取得できません", hr);
    UINT64 clockFrequency = 0;
    clock->GetFrequency(&clockFrequency);

    const UINT32 renderChannels = renderFormat->nChannels;
    const UINT32 captureChannels = captureFormat->nChannels;
    const UINT32 renderRate = renderFormat->nSamplesPerSec;
    const UINT32 captureRate = captureFormat->nSamplesPerSec;

    // サイン波（レベル）またはパルス（遅延）
    const double amplitude = std::pow(10.0, options.levelDb / 20.0);
    const double phaseStep = 2.0 * kPi * options.frequency / renderRate;
    double phase = 0.0;
    const UINT64 firstPulse = renderRate / 2;     // 0.5 秒後から
    const UINT64 pulseInterval = renderRate / 5;  // 200 ms ごと
    UINT64 renderedFrames = 0;

    auto fillRender = [&](UINT32 frames) -> HRESULT {
        BYTE* data = nullptr;
        HRESULT result = render->GetBuffer(frames, &data);
        if (FAILED(result))
            return result;
        auto* out = reinterpret_cast<float*>(data);
        for (UINT32 frame = 0; frame < frames; ++frame, ++renderedFrames)
        {
            float sample = 0.0f;
            if (options.latency)
            {
                if (renderedFrames >= firstPulse && (renderedFrames - firstPulse) % pulseInterval == 0)
                    sample = 0.5f;
            }
            else
            {
                sample = static_cast<float>(amplitude * std::sin(phase));
                phase += phaseStep;
                if (phase >= 2.0 * kPi)
                    phase -= 2.0 * kPi;
            }
            for (UINT32 ch = 0; ch < renderChannels; ++ch)
                out[frame * renderChannels + ch] = sample;
        }
        return render->ReleaseBuffer(frames, 0);
    };

    if (FAILED(hr = fillRender(renderBufferFrames)))
        return Fail(L"再生バッファに書き込めません", hr);

    std::vector<float> captured;
    captured.reserve(static_cast<size_t>((options.seconds + 1.0) * captureRate * captureChannels));
    std::vector<CapturePacket> packets;
    std::vector<double> renderOrigins;
    std::vector<double> captureOrigins;

    captureClient->Start();
    renderClient->Start();

    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(options.seconds * 1000.0))
    {
        UINT32 padding = 0;
        if (SUCCEEDED(renderClient->GetCurrentPadding(&padding)) && padding < renderBufferFrames)
        {
            if (FAILED(hr = fillRender(renderBufferFrames - padding)))
                return Fail(L"再生バッファに書き込めません", hr);
        }

        // 再生位置 0 が鳴った時刻 = いまの時刻 - いまの位置
        UINT64 position = 0;
        UINT64 positionQpc = 0;
        if (SUCCEEDED(clock->GetPosition(&position, &positionQpc)) && position > 0 && clockFrequency > 0)
            renderOrigins.push_back(static_cast<double>(positionQpc) * 1e-7 - static_cast<double>(position) / clockFrequency);

        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition)))
                return Fail(L"録音バッファを読めません", hr);

            packets.push_back({ captured.size() / captureChannels, devicePosition });
            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0)
                captureOrigins.push_back(static_cast<double>(qpcPosition) * 1e-7 - static_cast<double>(devicePosition) / captureRate);

            const size_t samples = static_cast<size_t>(frames) * captureChannels;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                captured.insert(captured.end(), samples, 0.0f);
            else
            {
                const auto* in = reinterpret_cast<const float*>(data);
                captured.insert(captured.end(), in, in + samples);
            }
            capture->ReleaseBuffer(frames);
        }
        Sleep(5);
    }

    renderClient->Stop();
    captureClient->Stop();

    if (!options.latency)
        return AnalyzeLevel(options, captured, captureChannels, captureRate);

    if (renderOrigins.empty() || captureOrigins.empty() || packets.empty())
    {
        std::fwprintf(stderr, L"error: タイムスタンプを取得できませんでした\n");
        return 1;
    }
    const LatencyInput input{ captured, captureChannels, captureRate, packets, Median(captureOrigins),
                              renderRate, Median(renderOrigins), firstPulse, pulseInterval, renderedFrames };
    return AnalyzeLatency(input);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    // 日本語のメッセージをコンソールに正しく出す
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");

    Options options;
    if (!ParseArgs(argc, argv, options))
    {
        PrintUsage();
        return 2;
    }

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return Fail(L"COM を初期化できません", hr);

    // デバイスが無効になった（AUDCLNT_E_DEVICE_INVALIDATED）ときは、デバイスを開き直して再試行する
    constexpr int kMaxAttempts = 4;
    int result = 1;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
    {
        g_lastError = S_OK;
        result = Run(options);
        if (result == 0 || g_lastError != AUDCLNT_E_DEVICE_INVALIDATED || attempt == kMaxAttempts)
            break;
        std::wprintf(L"retry  : デバイスが無効になったので開き直します（%d/%d）\n", attempt + 1, kMaxAttempts);
        Sleep(1000);
    }

    CoUninitialize();
    return result;
}
