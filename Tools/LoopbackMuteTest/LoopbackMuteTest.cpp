// LoopbackMuteTest: アプリの音をミュート（音量ミキサーのミュート / 音量 0）しても
// WASAPI Process Loopback で取り込めるかを確かめる。
//
//   LoopbackMuteTest.exe [--render "CABLE Input"] [--level -20]
//
// 自分自身を子プロセスとして起動し、子に 1 kHz を 10 秒流させる。子は自分のセッションの音量を
//   0〜3 秒: そのまま / 3〜6 秒: ミュート / 6〜9 秒: 音量 0 / 9〜10 秒: 元に戻す
// と変える。親はその子のプロセスツリーを Process Loopback で取り込み、1 秒ごとのレベルを出す。

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr double kToneSeconds = 10.0;

//==============================================================================
// 子: 指定デバイスにサイン波を流しながら、自分のセッションの音量を変える
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

int RunTone(const std::wstring& renderName, double levelDb)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDeviceCollection> collection;
    enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    UINT count = 0;
    collection->GetCount(&count);

    ComPtr<IMMDevice> device;
    for (UINT i = 0; i < count && !device; ++i)
    {
        ComPtr<IMMDevice> candidate;
        collection->Item(i, &candidate);
        if (FriendlyName(candidate.Get()).compare(0, renderName.size(), renderName) == 0)
            device = candidate;
    }
    if (!device)
        return 1;

    ComPtr<IAudioClient> client;
    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    WAVEFORMATEX* format = nullptr;
    client->GetMixFormat(&format);
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1'000'000, 0, format, nullptr)))
        return 1;

    ComPtr<IAudioRenderClient> render;
    ComPtr<ISimpleAudioVolume> volume; // = 音量ミキサーのこのアプリの音量とミュート
    client->GetService(IID_PPV_ARGS(&render));
    client->GetService(IID_PPV_ARGS(&volume));

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    const UINT32 channels = format->nChannels;
    const double step = 2.0 * kPi * 1000.0 / format->nSamplesPerSec;
    const double amplitude = std::pow(10.0, levelDb / 20.0);
    double phase = 0.0;

    auto fill = [&](UINT32 frames) {
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data)))
            return;
        auto* out = reinterpret_cast<float*>(data);
        for (UINT32 f = 0; f < frames; ++f)
        {
            const auto s = static_cast<float>(amplitude * std::sin(phase));
            phase += step;
            for (UINT32 c = 0; c < channels; ++c)
                out[f * channels + c] = s;
        }
        render->ReleaseBuffer(frames, 0);
    };

    fill(bufferFrames);
    client->Start();

    int stage = 0;
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(kToneSeconds * 1000))
    {
        const double t = (GetTickCount64() - start) / 1000.0;
        if (stage == 0 && t >= 3.0) { volume->SetMute(TRUE, nullptr); stage = 1; }
        if (stage == 1 && t >= 6.0) { volume->SetMute(FALSE, nullptr); volume->SetMasterVolume(0.0f, nullptr); stage = 2; }
        if (stage == 2 && t >= 9.0) { volume->SetMasterVolume(1.0f, nullptr); stage = 3; }

        UINT32 padding = 0;
        if (SUCCEEDED(client->GetCurrentPadding(&padding)) && padding < bufferFrames)
            fill(bufferFrames - padding);
        Sleep(5);
    }

    client->Stop();
    CoTaskMemFree(format);
    return 0;
}

//==============================================================================
// 親: 子のプロセスツリーを Process Loopback で取り込む
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() { CloseHandle(done); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *ppv = static_cast<IAgileObject*>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refCount)); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG c = InterlockedDecrement(&refCount);
        if (c == 0)
            delete this;
        return static_cast<ULONG>(c);
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT activateResult = E_FAIL;
        ComPtr<IUnknown> unknown;
        result = operation->GetActivateResult(&activateResult, &unknown);
        if (SUCCEEDED(result))
            result = activateResult;
        if (SUCCEEDED(result))
            result = unknown.As(&audioClient);
        SetEvent(done);
        return S_OK;
    }

    HANDLE done = nullptr;
    HRESULT result = E_FAIL;
    ComPtr<IAudioClient> audioClient;

private:
    LONG refCount = 1;
};

int RunCapture(const std::wstring& renderName, double levelDb)
{
    // 子を起動する
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t level[32];
    swprintf_s(level, L"%.1f", levelDb);
    std::wstring commandLine = L"\"" + std::wstring(self) + L"\" --child --render \"" + renderName + L"\" --level " + level;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        std::wprintf(L"error: cannot start child (%lu)\n", GetLastError());
        return 1;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pi.dwProcessId;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, handler, &operation);
    if (SUCCEEDED(hr))
        hr = WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0 ? handler->result : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    ComPtr<IAudioClient> client = handler->audioClient;
    handler->Release();
    if (FAILED(hr))
    {
        std::wprintf(L"error: process loopback activation failed (0x%08X)\n", static_cast<unsigned>(hr));
        return 1;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = kChannels * sizeof(float);
    format.nAvgBytesPerSec = kSampleRate * format.nBlockAlign;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            200000, 0, &format, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(hr) || FAILED(client->GetService(IID_PPV_ARGS(&capture))))
    {
        std::wprintf(L"error: initialize failed (0x%08X)\n", static_cast<unsigned>(hr));
        return 1;
    }
    client->Start();

    // 1 秒ごとの RMS
    std::vector<double> sumSquares(12, 0.0);
    std::vector<size_t> frameCounts(12, 0);
    const ULONGLONG start = GetTickCount64();
    while (WaitForSingleObject(pi.hProcess, 5) == WAIT_TIMEOUT || GetTickCount64() - start < 1000)
    {
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            const size_t second = std::min<size_t>(11, (GetTickCount64() - start) / 1000);
            const auto* samples = reinterpret_cast<const float*>(data);
            for (UINT32 f = 0; f < frames; ++f)
            {
                const double s = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? 0.0 : samples[f * kChannels];
                sumSquares[second] += s * s;
            }
            frameCounts[second] += frames;
            capture->ReleaseBuffer(frames);
        }
        if (GetTickCount64() - start > 13000)
            break;
    }
    client->Stop();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    const double sentRmsDb = levelDb - 3.0103;
    const wchar_t* stages[] = { L"normal", L"normal", L"normal", L"MUTE", L"MUTE", L"MUTE", L"VOLUME 0", L"VOLUME 0", L"VOLUME 0", L"normal", L"-", L"-" };
    for (size_t i = 0; i < 11; ++i)
    {
        if (frameCounts[i] == 0)
            continue;
        const double rms = std::sqrt(sumSquares[i] / frameCounts[i]);
        const double db = 20.0 * std::log10(rms > 1e-10 ? rms : 1e-10);
        std::wprintf(L"%2zu-%2zu s  %-9ls  %8.2f dBFS RMS (送った音との差 %+7.2f dB)\n", i, i + 1, stages[i], db, db - sentRmsDb);
    }
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool child = false;
    std::wstring renderName = L"CABLE Input";
    double levelDb = -20.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--child")
            child = true;
        else if (arg == L"--render" && i + 1 < argc)
            renderName = argv[++i];
        else if (arg == L"--level" && i + 1 < argc)
            levelDb = _wtof(argv[++i]);
    }

    const int result = child ? RunTone(renderName, levelDb) : RunCapture(renderName, levelDb);
    CoUninitialize();
    return result;
}
