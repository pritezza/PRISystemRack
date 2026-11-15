#include "PriApo.h"

#include "ApoGuids.h"
#include "ApoLog.h"
#include "Module.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace pri::apo
{
namespace
{
// PKEY_AudioEndpoint_GUID（mmdeviceapi.h）。INITGUID に頼らずに使うためここで定義する
constexpr PROPERTYKEY kPkeyAudioEndpointGuid = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4 };

std::atomic<long> g_instanceCounter{ 0 };

// 対応していないインターフェイスの問い合わせを、このプロセスで初めてのときだけ true にする。
// Windows は再生中も数秒おきに同じ問い合わせ（{24BDC606-...} など。SDK にない GUID）を繰り返し、
// 毎回ログに書くとログの 9 割がこの行になっていた（2026-11-13、apo.log 8.5 MB のうち 66,001 行）
bool FirstUnsupportedQuery(REFIID riid)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::vector<GUID> seen;

    AcquireSRWLockExclusive(&lock);
    const bool first = std::find(seen.begin(), seen.end(), riid) == seen.end();
    if (first && seen.size() < 256)
        seen.push_back(riid);
    ReleaseSRWLockExclusive(&lock);
    return first;
}

// HKLM\SOFTWARE\PRISystemRack\APO の TestGainDb（文字列。例: "-6"）を読む。なければ 0 dB
double ReadTestGainDb()
{
    wchar_t text[64] = {};
    DWORD size = sizeof(text);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, kApoRegistryKey, L"TestGainDb",
                                        RRF_RT_REG_SZ, nullptr, text, &size);
    if (status != ERROR_SUCCESS)
        return 0.0;

    const double db = std::wcstod(text, nullptr);
    if (!std::isfinite(db))
        return 0.0;
    return std::clamp(db, -60.0, 12.0);
}

// 本体の結果を待つ時間の上限。HKLM\SOFTWARE\PRISystemRack\APO の HostTimeoutMicros（DWORD）で上書きできる
uint32_t ReadHostTimeoutMicros(uint32_t defaultMicros)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, kApoRegistryKey, L"HostTimeoutMicros",
                                        RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status != ERROR_SUCCESS || value < 100 || value > 100'000)
        return defaultMicros;
    return value;
}

// ログ用: フォーマットを "2ch 48000Hz float32/32" のような文字列にする
void DescribeFormat(IAudioMediaType* type, wchar_t* text, size_t count)
{
    if (type == nullptr)
    {
        wcsncpy_s(text, count, L"(null)", _TRUNCATE);
        return;
    }

    UNCOMPRESSEDAUDIOFORMAT format = {};
    const HRESULT hr = type->GetUncompressedAudioFormat(&format);
    if (FAILED(hr))
    {
        swprintf_s(text, count, L"(GetUncompressedAudioFormat hr=0x%08X)", static_cast<unsigned>(hr));
        return;
    }

    const bool isFloat = IsEqualGUID(format.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    const bool isPcm = IsEqualGUID(format.guidFormatType, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    swprintf_s(text, count, L"%luch %.0fHz %ls%lu/%lu mask=0x%lX",
               format.dwSamplesPerFrame, static_cast<double>(format.fFramesPerSecond),
               isFloat ? L"float" : (isPcm ? L"pcm" : L"other"),
               format.dwValidBitsPerSample, format.dwBytesPerSampleContainer * 8, format.dwChannelMask);
}
} // namespace

// CRegAPOProperties<1> は追加のインターフェース用にサイズ 0 の配列を持つ（SDK の作り）ので C4815 を抑える
#pragma warning(push)
#pragma warning(disable : 4815)
const CRegAPOProperties<1> PriApo::kRegProperties(
    kClsidPriSystemRackApo, kApoFriendlyName, L"Copyright (C) 2026 PRISystemRack", 0, 1,
    __uuidof(IAudioProcessingObject),
    static_cast<APO_FLAG>(APO_FLAG_DEFAULT | APO_FLAG_INPLACE));
#pragma warning(pop)

PriApo::PriApo(IUnknown* outer)
    : CBaseAudioProcessingObject(kRegProperties),
      instanceId_(++g_instanceCounter)
{
    outer_ = outer != nullptr ? outer : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    ++g_objectCount;
    Log(L"#%ld created (aggregated=%d)", instanceId_, outer != nullptr ? 1 : 0);
}

PriApo::~PriApo()
{
    Log(L"#%ld destroyed", instanceId_);
    --g_objectCount;
}

//==============================================================================
// IUnknown

STDMETHODIMP PriApo::QueryInterface(REFIID riid, void** ppv)
{
    return outer_->QueryInterface(riid, ppv);
}

STDMETHODIMP_(ULONG) PriApo::AddRef()
{
    return outer_->AddRef();
}

STDMETHODIMP_(ULONG) PriApo::Release()
{
    return outer_->Release();
}

HRESULT STDMETHODCALLTYPE PriApo::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    if (ppv == nullptr)
        return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown))
        *ppv = static_cast<INonDelegatingUnknown*>(this);
    else if (riid == __uuidof(IAudioProcessingObject))
        *ppv = static_cast<IAudioProcessingObject*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectRT))
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    else if (riid == __uuidof(IAudioSystemEffects))
        *ppv = static_cast<IAudioSystemEffects*>(this);
    else
    {
        if (FirstUnsupportedQuery(riid))
        {
            wchar_t iid[40] = {};
            StringFromGUID2(riid, iid, _countof(iid));
            Log(L"#%ld QueryInterface: not supported %ls (logged once per process)", instanceId_, iid);
        }
        return E_NOINTERFACE;
    }

    // IUnknown のときは NonDelegatingAddRef、それ以外は外側への AddRef が呼ばれる
    static_cast<IUnknown*>(*ppv)->AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE PriApo::NonDelegatingAddRef()
{
    return ++refCount_;
}

ULONG STDMETHODCALLTYPE PriApo::NonDelegatingRelease()
{
    const ULONG count = --refCount_;
    if (count == 0)
        delete this;
    return count;
}

//==============================================================================
// IAudioProcessingObject

STDMETHODIMP PriApo::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    if (pbyData == nullptr && cbDataSize != 0)
        return E_INVALIDARG;
    if (pbyData != nullptr && cbDataSize == 0)
        return E_POINTER;

    // LFX/GFX の枠では APOInitSystemEffects、SFX/MFX/EFX の枠では APOInitSystemEffects2 以降が渡される。
    // どちらも先頭の並びは同じなので、APOInitSystemEffects の範囲だけ読む。
    if (pbyData != nullptr && cbDataSize >= sizeof(APOInitSystemEffects))
    {
        const auto* init = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
        if (init->pAPOEndpointProperties != nullptr)
        {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(init->pAPOEndpointProperties->GetValue(kPkeyAudioEndpointGuid, &value))
                && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            {
                // 共有メモリの名前に使うので大文字にそろえる（本体アプリ側も同じ）
                wcsncpy_s(endpointGuid_, value.pwszVal, _TRUNCATE);
                for (wchar_t* c = endpointGuid_; *c != L'\0'; ++c)
                    *c = static_cast<wchar_t>(towupper(*c));
            }
            PropVariantClear(&value);
        }
    }

    m_bIsInitialized = true;
    Log(L"#%ld Initialize: cbDataSize=%u endpoint=%ls", instanceId_, cbDataSize,
        endpointGuid_[0] != L'\0' ? endpointGuid_ : L"(unknown)");
    return S_OK;
}

STDMETHODIMP PriApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
                                            IAudioMediaType** ppSupportedInputFormat)
{
    const HRESULT hr = CBaseAudioProcessingObject::IsInputFormatSupported(
        pOppositeFormat, pRequestedInputFormat, ppSupportedInputFormat);

    wchar_t opposite[96];
    wchar_t requested[96];
    wchar_t supported[96];
    DescribeFormat(pOppositeFormat, opposite, _countof(opposite));
    DescribeFormat(pRequestedInputFormat, requested, _countof(requested));
    DescribeFormat(ppSupportedInputFormat != nullptr ? *ppSupportedInputFormat : nullptr, supported, _countof(supported));
    Log(L"#%ld IsInputFormatSupported: output=[%ls] requested=[%ls] -> hr=0x%08X supported=[%ls]",
        instanceId_, opposite, requested, static_cast<unsigned>(hr), supported);
    return hr;
}

STDMETHODIMP PriApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
                                             IAudioMediaType** ppSupportedOutputFormat)
{
    const HRESULT hr = CBaseAudioProcessingObject::IsOutputFormatSupported(
        pOppositeFormat, pRequestedOutputFormat, ppSupportedOutputFormat);

    wchar_t opposite[96];
    wchar_t requested[96];
    wchar_t supported[96];
    DescribeFormat(pOppositeFormat, opposite, _countof(opposite));
    DescribeFormat(pRequestedOutputFormat, requested, _countof(requested));
    DescribeFormat(ppSupportedOutputFormat != nullptr ? *ppSupportedOutputFormat : nullptr, supported, _countof(supported));
    Log(L"#%ld IsOutputFormatSupported: input=[%ls] requested=[%ls] -> hr=0x%08X supported=[%ls]",
        instanceId_, opposite, requested, static_cast<unsigned>(hr), supported);
    return hr;
}

STDMETHODIMP PriApo::Reset()
{
    Log(L"#%ld Reset", instanceId_);
    return CBaseAudioProcessingObject::Reset();
}

//==============================================================================
// IAudioProcessingObjectConfiguration

STDMETHODIMP PriApo::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                    UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    const HRESULT hr = CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr))
    {
        Log(L"#%ld LockForProcess: base failed hr=0x%08X", instanceId_, static_cast<unsigned>(hr));
        return hr;
    }

    channels_ = GetSamplesPerFrame();
    const double gainDb = ReadTestGainDb();
    gain_ = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    framesProcessed_ = 0;
    processCalls_ = 0;
    validBuffers_ = 0;
    silentBuffers_ = 0;
    hostProcessed_ = 0;
    hostTimedOut_ = 0;
    hostAbsent_ = 0;

    const APO_CONNECTION_DESCRIPTOR* input = ppInputConnections[0];
    const APO_CONNECTION_DESCRIPTOR* output = ppOutputConnections[0];
    const auto sampleRate = static_cast<uint32_t>(GetFramesPerSecond());
    const UINT32 maxFrames = input->u32MaxFrameCount;
    Log(L"#%ld LockForProcess: endpoint=%ls channels=%u rate=%u maxFrames=%u inPlace=%d testGain=%.2f dB",
        instanceId_, endpointGuid_[0] != L'\0' ? endpointGuid_ : L"(unknown)", channels_, sampleRate, maxFrames,
        input->pBuffer == output->pBuffer ? 1 : 0, gainDb);

    // 本体アプリとの受け渡し口を開く。待ち時間の上限は既定で 1 周期の 60%
    if (sampleRate > 0)
    {
        const auto periodMicros = static_cast<uint32_t>(1'000'000ull * maxFrames / sampleRate);
        const uint32_t timeoutMicros = ReadHostTimeoutMicros(periodMicros * 6 / 10);
        const uint64_t ownerToken = (static_cast<uint64_t>(GetCurrentProcessId()) << 32)
                                    | static_cast<uint32_t>(instanceId_);
        host_.Open(endpointGuid_, ownerToken, sampleRate, channels_, maxFrames, timeoutMicros);
    }
    return S_OK;
}

STDMETHODIMP PriApo::UnlockForProcess()
{
    Log(L"#%ld UnlockForProcess: calls=%llu valid=%llu silent=%llu frames=%llu host: processed=%llu timedOut=%llu absent=%llu",
        instanceId_, processCalls_.load(), validBuffers_.load(), silentBuffers_.load(), framesProcessed_.load(),
        hostProcessed_.load(), hostTimedOut_.load(), hostAbsent_.load());
    host_.Close();
    return CBaseAudioProcessingObject::UnlockForProcess();
}

//==============================================================================
// IAudioProcessingObjectRT（オーディオエンジンのリアルタイムスレッドから呼ばれる。
// メモリ確保、ロック、ファイル操作、ログ出力をしないこと）

#pragma AVRT_CODE_BEGIN
STDMETHODIMP_(void) PriApo::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                       UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    ASSERT_REALTIME();
    processCalls_.fetch_add(1, std::memory_order_relaxed);
    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0)
        return;

    APO_CONNECTION_PROPERTY* input = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* output = ppOutputConnections[0];
    const UINT32 frames = input->u32ValidFrameCount;
    const auto* source = reinterpret_cast<const float*>(input->pBuffer);
    auto* destination = reinterpret_cast<float*>(output->pBuffer);
    const size_t samples = static_cast<size_t>(frames) * channels_;
    const bool isValid = input->u32BufferFlags == BUFFER_VALID;
    const bool isSilent = input->u32BufferFlags == BUFFER_SILENT;
    if (isValid)
        validBuffers_.fetch_add(1, std::memory_order_relaxed);
    else if (isSilent)
        silentBuffers_.fetch_add(1, std::memory_order_relaxed);

    // 本体アプリに処理してもらう。無音のときも渡す（リバーブの残響やアナライザーの表示を途切れさせない）
    if ((isValid || isSilent) && host_.IsActive())
    {
        switch (host_.Process(isSilent ? nullptr : source, destination, frames))
        {
        case HostChannel::Result::Processed:
            hostProcessed_.fetch_add(1, std::memory_order_relaxed);
            output->u32BufferFlags = BUFFER_VALID;
            output->u32ValidFrameCount = frames;
            framesProcessed_.fetch_add(frames, std::memory_order_relaxed);
            return;
        case HostChannel::Result::TimedOut:
            hostTimedOut_.fetch_add(1, std::memory_order_relaxed);
            break;
        case HostChannel::Result::NoHost:
            hostAbsent_.fetch_add(1, std::memory_order_relaxed);
            break;
        case HostChannel::Result::Unavailable:
            break;
        }
    }

    // 素通し（確認用の TestGainDb だけかける）
    switch (input->u32BufferFlags)
    {
    case BUFFER_VALID:
        if (gain_ == 1.0f)
        {
            if (destination != source)
                std::memcpy(destination, source, samples * sizeof(float));
        }
        else
        {
            const float gain = gain_;
            for (size_t i = 0; i < samples; ++i)
                destination[i] = source[i] * gain;
        }
        output->u32BufferFlags = BUFFER_VALID;
        break;

    case BUFFER_SILENT:
        if (destination != source)
            std::memset(destination, 0, samples * sizeof(float));
        output->u32BufferFlags = BUFFER_SILENT;
        break;

    default:
        output->u32BufferFlags = input->u32BufferFlags;
        break;
    }

    output->u32ValidFrameCount = frames;
    framesProcessed_.fetch_add(frames, std::memory_order_relaxed);
}
#pragma AVRT_CODE_END
} // namespace pri::apo
