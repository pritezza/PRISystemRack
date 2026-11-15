#include "ProcessCapture.h"

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwctype>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pri::host
{
namespace
{
// ActivateAudioInterfaceAsync の完了を受け取る。IAgileObject を実装していないと E_ILLEGAL_METHOD_CALL になる
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() { CloseHandle(done); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
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
        const LONG count = InterlockedDecrement(&refCount);
        if (count == 0)
            delete this;
        return static_cast<ULONG>(count);
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

// 有効化の完了を最大 5 秒待つ。stopRequested が立ったら待つのをやめる（Stop をすぐ終わらせるため）
HRESULT ActivateProcessLoopback(DWORD processId, ComPtr<IAudioClient>& client, const std::atomic<bool>& stopRequested)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = processId;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams = {};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams,
                                             handler, &operation);
    if (SUCCEEDED(hr))
    {
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        for (int waited = 0; waited < 5000 && !stopRequested.load(); waited += 50)
        {
            if (WaitForSingleObject(handler->done, 50) == WAIT_OBJECT_0)
            {
                hr = handler->result;
                break;
            }
        }
        if (stopRequested.load() && hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (SUCCEEDED(hr))
        client = handler->audioClient;
    handler->Release(); // 待つのをやめても、完了の通知は操作が持つ参照で安全に届く
    return hr;
}
} // namespace

//==============================================================================
ProcessCapture::~ProcessCapture()
{
    Stop();
}

void ProcessCapture::Start(uint32_t processId, uint32_t sampleRate, uint32_t channels, Callback callback)
{
    Stop();

    processId_ = processId;
    sampleRate_ = sampleRate;
    channels_ = channels;
    callback_ = std::move(callback);
    stopRequested_ = false;
    {
        std::lock_guard<std::mutex> lock(errorLock_);
        lastError_.clear();
    }

    state_ = State::starting;
    thread_ = std::thread([this] { Run(); });
}

void ProcessCapture::Stop()
{
    stopRequested_ = true;
    if (thread_.joinable())
        thread_.join();
    state_ = State::stopped;
}

std::wstring ProcessCapture::GetLastError() const
{
    std::lock_guard<std::mutex> lock(errorLock_);
    return lastError_;
}

void ProcessCapture::SetError(const wchar_t* message, long hr)
{
    wchar_t text[256];
    swprintf_s(text, L"%ls (0x%08lX)", message, static_cast<unsigned long>(hr));
    std::lock_guard<std::mutex> lock(errorLock_);
    lastError_ = text;
}

void ProcessCapture::Run()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HANDLE ready = nullptr;
    {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        HRESULT hr = ActivateProcessLoopback(processId_, client, stopRequested_);

        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = static_cast<WORD>(channels_);
        format.nSamplesPerSec = sampleRate_;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(channels_ * sizeof(float));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        if (FAILED(hr))
            SetError(L"process loopback activation failed", hr);
        // Process Loopback は GetMixFormat が使えないので、欲しい形式を指定して変換させる
        else if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                200000, 0, &format, nullptr)))
            SetError(L"IAudioClient::Initialize failed", hr);
        else if ((ready = CreateEventW(nullptr, FALSE, FALSE, nullptr)) == nullptr)
            SetError(L"CreateEvent failed", HRESULT_FROM_WIN32(::GetLastError()));
        else if (FAILED(hr = client->SetEventHandle(ready)))
            SetError(L"IAudioClient::SetEventHandle failed", hr);
        else if (FAILED(hr = client->GetService(IID_PPV_ARGS(&capture))))
            SetError(L"IAudioCaptureClient is not available", hr);
        else if (FAILED(hr = client->Start()))
            SetError(L"IAudioClient::Start failed", hr);
        else
        {
            state_ = State::running;

            DWORD taskIndex = 0;
            const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            std::vector<float> silence;

            while (!stopRequested_)
            {
                const DWORD wait = WaitForSingleObject(ready, 100);
                if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
                {
                    SetError(L"wait failed", HRESULT_FROM_WIN32(::GetLastError()));
                    break;
                }

                UINT32 packetFrames = 0;
                while (SUCCEEDED(hr = capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0)
                {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                        break;

                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                    {
                        silence.assign(static_cast<size_t>(frames) * channels_, 0.0f);
                        callback_(silence.data(), frames, channels_);
                    }
                    else
                    {
                        callback_(reinterpret_cast<const float*>(data), frames, channels_);
                    }
                    capture->ReleaseBuffer(frames);
                }

                if (FAILED(hr))
                {
                    SetError(L"capture failed", hr);
                    break;
                }
            }

            client->Stop();
            if (mmcss != nullptr)
                AvRevertMmThreadCharacteristics(mmcss);
        }
    }

    if (ready != nullptr)
        CloseHandle(ready);
    if (SUCCEEDED(comResult))
        CoUninitialize();

    // Stop で抜けたときは Stop が stopped にする。それ以外（準備や取り込みの失敗）は failed
    if (!stopRequested_.load())
        state_ = State::failed;
}

//==============================================================================
SilentStream::~SilentStream()
{
    Stop();
}

void SilentStream::Start(const std::wstring& endpointGuid)
{
    if (thread_.joinable() && endpointGuid == endpointGuid_)
        return;

    Stop();
    endpointGuid_ = endpointGuid;
    stopRequested_ = false;
    thread_ = std::thread([this] { Run(); });
}

void SilentStream::Stop()
{
    stopRequested_ = true;
    if (thread_.joinable())
        thread_.join();
}

void SilentStream::Run()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // デバイスが消えたり無効になったりしたら、1 秒おきに開き直す
    while (!stopRequested_)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEX* format = nullptr;
        UINT32 bufferFrames = 0;

        // MMDevice の ID は "{0.0.0.00000000}.{エンドポイントの GUID（小文字）}"
        std::wstring id = L"{0.0.0.00000000}." + endpointGuid_;
        for (wchar_t& c : id)
            c = static_cast<wchar_t>(towlower(c));

        const bool opened = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))
                         && SUCCEEDED(enumerator->GetDevice(id.c_str(), &device))
                         && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())))
                         && SUCCEEDED(client->GetMixFormat(&format))
                         && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1'000'000, 0, format, nullptr))
                         && SUCCEEDED(client->GetBufferSize(&bufferFrames))
                         && SUCCEEDED(client->GetService(IID_PPV_ARGS(&render)))
                         && SUCCEEDED(client->Start());

        bool failed = !opened;
        while (!failed && !stopRequested_)
        {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)))
            {
                failed = true;
                break;
            }

            const UINT32 frames = bufferFrames - padding;
            BYTE* data = nullptr;
            if (frames > 0)
            {
                if (FAILED(render->GetBuffer(frames, &data)) || FAILED(render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT)))
                {
                    failed = true;
                    break;
                }
            }
            Sleep(20);
        }

        if (client)
            client->Stop();
        if (format != nullptr)
            CoTaskMemFree(format);

        for (int i = 0; i < 10 && !stopRequested_; ++i)
            Sleep(100);
    }

    if (SUCCEEDED(comResult))
        CoUninitialize();
}
} // namespace pri::host
