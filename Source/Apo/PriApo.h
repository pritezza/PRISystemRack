#pragma once

#include <windows.h>

#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>

#include <atomic>

#include "HostChannel.h"

namespace pri::apo
{
// COM の集約（aggregation）用。外側のオブジェクトに委譲しない IUnknown。
// vtable の並びが IUnknown と同じなので、IUnknown* として扱える。
struct INonDelegatingUnknown
{
    virtual HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG STDMETHODCALLTYPE NonDelegatingAddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE NonDelegatingRelease() = 0;
};

// ミックス後（post-mix）の枠に入れる APO。
// 本体アプリがいれば、毎周期の音を共有メモリで渡して処理結果を受け取る（HostChannel）。
// 本体がいない、または時間内に戻らなければ素通しする。
// 確認用に、素通しのときだけレジストリの TestGainDb で指定したゲインをかける。
class PriApo final : public CBaseAudioProcessingObject,
                     public IAudioSystemEffects,
                     public INonDelegatingUnknown
{
public:
    explicit PriApo(IUnknown* outer);
    ~PriApo() override;

    PriApo(const PriApo&) = delete;
    PriApo& operator=(const PriApo&) = delete;

    static const CRegAPOProperties<1> kRegProperties;

    // IUnknown（外側のオブジェクトに委譲する）
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // INonDelegatingUnknown
    HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE NonDelegatingAddRef() override;
    ULONG STDMETHODCALLTYPE NonDelegatingRelease() override;

    // IAudioProcessingObject
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
                                        IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
                                         IAudioMediaType** ppSupportedOutputFormat) override;
    STDMETHODIMP Reset() override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

    // IAudioProcessingObjectRT
    STDMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                   UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections) override;

private:
    IUnknown* outer_ = nullptr; // 集約されていなければ自分自身（INonDelegatingUnknown）
    std::atomic<ULONG> refCount_{ 1 };

    // LockForProcess で決まり、APOProcess で読む
    UINT32 channels_ = 0;
    float gain_ = 1.0f;

    // APOProcess で数え、UnlockForProcess でログに出す
    std::atomic<UINT64> framesProcessed_{ 0 };
    std::atomic<UINT64> processCalls_{ 0 };
    std::atomic<UINT64> validBuffers_{ 0 };
    std::atomic<UINT64> silentBuffers_{ 0 };
    std::atomic<UINT64> hostProcessed_{ 0 };
    std::atomic<UINT64> hostTimedOut_{ 0 };
    std::atomic<UINT64> hostAbsent_{ 0 };

    HostChannel host_;

    // ログで見分けるための通し番号
    const long instanceId_;

    wchar_t endpointGuid_[64] = {};
};
} // namespace pri::apo
