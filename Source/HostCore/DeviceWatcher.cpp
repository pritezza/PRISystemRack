#include "DeviceWatcher.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace pri::host
{
namespace
{
// 再生デバイスの ID は "{0.0.0.00000000}.{GUID}"、録音デバイスは "{0.0.1.00000000}.{GUID}" で始まる
bool IsRenderDeviceId(LPCWSTR deviceId)
{
    return deviceId != nullptr && std::wcsncmp(deviceId, L"{0.0.0.", 7) == 0;
}

class NotificationClient final : public IMMNotificationClient
{
public:
    std::atomic<uint32_t> changes{ 0 };

    // IUnknown（寿命は DeviceWatcher が持つので、参照の数は数えない）
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        {
            *object = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD) override { count(deviceId); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override { count(deviceId); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override { count(deviceId); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY) override { count(deviceId); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; } // 既定のデバイスは一覧に出さない

private:
    void count(LPCWSTR deviceId)
    {
        if (IsRenderDeviceId(deviceId))
            changes.fetch_add(1, std::memory_order_relaxed);
    }
};
} // namespace

struct DeviceWatcher::Impl
{
    bool comInitialised = false;
    ComPtr<IMMDeviceEnumerator> enumerator;
    NotificationClient client;
    bool registered = false;
};

DeviceWatcher::DeviceWatcher() : impl(std::make_unique<Impl>())
{
    impl->comInitialised = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&impl->enumerator))))
        return;

    impl->registered = SUCCEEDED(impl->enumerator->RegisterEndpointNotificationCallback(&impl->client));
}

DeviceWatcher::~DeviceWatcher()
{
    if (impl->registered)
        impl->enumerator->UnregisterEndpointNotificationCallback(&impl->client);

    impl->enumerator.Reset();

    if (impl->comInitialised)
        CoUninitialize();
}

bool DeviceWatcher::IsWatching() const
{
    return impl->registered;
}

uint32_t DeviceWatcher::GetChangeCount() const
{
    return impl->client.changes.load(std::memory_order_relaxed);
}
} // namespace pri::host
