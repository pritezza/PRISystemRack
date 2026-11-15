#include "Endpoints.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cwchar>
#include <cwctype>
#include <vector>

#include "Common/FxSlots.h"
#include "Common/RackIpc.h"

using Microsoft::WRL::ComPtr;

namespace ipc = pri::ipc;
namespace fx = pri::fx;

namespace pri::host
{
namespace
{
// INITGUID に頼らずに使うためここで定義する
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
constexpr PROPERTYKEY kPkeyAudioEndpointGuid = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4 };
// エンドポイントの親（音声デバイス）のインスタンス ID。例: "{1}.USB\VID_0499&PID_170D\7&1F3E9D8&0&1"。
// レートごとのエンドポイントでも同じ値になる（2026-11-11、AG06 の 44.1 kHz と 48 kHz のエンドポイントで確認）
constexpr PROPERTYKEY kPkeyEndpointParentInstance = {
    { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 2 };

class RegKey
{
public:
    explicit RegKey(const std::wstring& path)
    {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key_) != ERROR_SUCCESS)
            key_ = nullptr;
    }
    ~RegKey()
    {
        if (key_ != nullptr)
            RegCloseKey(key_);
    }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    HKEY Get() const { return key_; }

private:
    HKEY key_ = nullptr;
};

// 文字列の値（REG_SZ）または文字列の並び（REG_MULTI_SZ）を読む。並びは '\0' で区切ったまま、最後に '\0' が 2 つ続く
bool ReadStrings(HKEY key, const wchar_t* valueName, std::vector<wchar_t>& text)
{
    if (key == nullptr)
        return false;

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_MULTI_SZ))
        return false;

    text.assign(size / sizeof(wchar_t) + 2, L'\0'); // 終端の '\0' がない値もあるので、2 つ余分に取っておく
    return RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &size) == ERROR_SUCCESS;
}

// その値が PRISystemRack 以外の APO を指していれば true（Composite の枠は CLSID の並び）
bool HoldsOtherApo(HKEY key, const wchar_t* valueName)
{
    std::vector<wchar_t> text;
    if (!ReadStrings(key, valueName, text))
        return false;

    for (const wchar_t* item = text.data(); *item != L'\0'; item += wcslen(item) + 1)
        if (_wcsicmp(item, ipc::kApoClsidString) != 0)
            return true;
    return false;
}

// 設定画面に出る名前（分からなければ空）
std::wstring ReadApoName(HKEY key)
{
    std::vector<wchar_t> text;
    return ReadStrings(key, fx::kNameValue, text) ? std::wstring(text.data()) : std::wstring();
}

std::wstring FxPropertiesPath(const std::wstring& guid)
{
    return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid + L"\\FxProperties";
}

// FxProperties のミックス後の枠（PKEY_FX_PostMixEffectClsid）を見る
bool IsApoInstalled(HKEY fxProperties)
{
    std::vector<wchar_t> text;
    return ReadStrings(fxProperties, fx::kPostMixValue, text) && _wcsicmp(text.data(), ipc::kApoClsidString) == 0;
}

// key の APO の枠のどれかに PRISystemRack 以外の APO があれば true にし、名前を返す（Setup ツールが控えて外す枠と同じ一覧）
std::wstring FindOtherApo(HKEY key, bool& present)
{
    present = false;
    for (const wchar_t* slot : fx::kApoSlotValues)
        present = present || HoldsOtherApo(key, slot);
    return present ? ReadApoName(key) : std::wstring();
}

std::wstring ReadString(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
        text = value.pwszVal;
    PropVariantClear(&value);
    return text;
}
} // namespace

std::vector<RenderEndpoint> ListRenderEndpoints(bool includeInactive)
{
    std::vector<RenderEndpoint> endpoints;

    // このスレッドで COM がまだなら初期化する（すでに別の方式で初期化済みでもそのまま使える）
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope
    {
        bool initialised;
        ~ComScope()
        {
            if (initialised)
                CoUninitialize();
        }
    } comScope{ SUCCEEDED(comResult) };

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return endpoints;

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, includeInactive ? DEVICE_STATEMASK_ALL : DEVICE_STATE_ACTIVE, &collection)))
        return endpoints;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> store;
        if (FAILED(collection->Item(i, &device)) || FAILED(device->OpenPropertyStore(STGM_READ, &store)))
            continue;

        RenderEndpoint endpoint;
        endpoint.friendlyName = ReadString(store.Get(), kPkeyDeviceFriendlyName);
        endpoint.guid = ReadString(store.Get(), kPkeyAudioEndpointGuid);
        for (wchar_t& c : endpoint.guid)
            c = static_cast<wchar_t>(towupper(c));
        if (endpoint.guid.empty())
            continue;

        DWORD state = 0;
        endpoint.active = SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;

        // 親のインスタンス ID と、表示名（"Line (AG06/AG03)" のように、出力の名前とドライバーの名前を含む）を組にする。
        // 表示名まで比べるのは、HDMI のように同じ親の下にモニターやドライバーごとのエンドポイントが並ぶものを、別のものとして扱うため。
        // 親のインスタンス ID は大文字小文字が揺れることがあるので大文字にそろえる
        auto parent = ReadString(store.Get(), kPkeyEndpointParentInstance);
        for (wchar_t& c : parent)
            c = static_cast<wchar_t>(towupper(c));
        if (!parent.empty() && !endpoint.friendlyName.empty())
            endpoint.deviceKey = parent + L"|" + endpoint.friendlyName;
        {
            const RegKey endpointKey(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + endpoint.guid);
            endpoint.canHaveApo = endpointKey.Get() != nullptr;
        }
        {
            const RegKey fxProperties(FxPropertiesPath(endpoint.guid));
            endpoint.apoInstalled = IsApoInstalled(fxProperties.Get());
            endpoint.otherApo = FindOtherApo(fxProperties.Get(), endpoint.otherApoPresent);
        }
        {
            // PRISystemRack を入れたときに控えた元の登録（外すとこれが戻る）
            const RegKey backup(std::wstring(fx::kBackupRoot) + L"\\" + endpoint.guid);
            endpoint.backupPresent = backup.Get() != nullptr;
            endpoint.backupApo = FindOtherApo(backup.Get(), endpoint.backupHasApo);
        }
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

std::vector<RenderEndpoint> FindSiblingEndpoints(const std::wstring& guid)
{
    const auto all = ListRenderEndpoints(true);
    std::wstring key;
    for (const auto& endpoint : all)
        if (_wcsicmp(endpoint.guid.c_str(), guid.c_str()) == 0)
            key = endpoint.deviceKey;

    std::vector<RenderEndpoint> siblings;
    if (key.empty())
        return siblings;

    for (const auto& endpoint : all)
        if (endpoint.deviceKey == key && _wcsicmp(endpoint.guid.c_str(), guid.c_str()) != 0)
            siblings.push_back(endpoint);
    return siblings;
}

std::optional<RenderEndpoint> FindActiveSuccessor(const std::wstring& guid)
{
    std::optional<RenderEndpoint> found;
    for (auto& endpoint : FindSiblingEndpoints(guid))
    {
        if (!endpoint.active)
            continue;
        if (found)
            return std::nullopt; // 同じ出力が 2 つ以上使える。どれか決められない
        found = std::move(endpoint);
    }
    return found;
}

std::optional<RenderEndpoint> FindRenderEndpoint(const std::wstring& namePrefix)
{
    std::optional<RenderEndpoint> found;
    for (auto& endpoint : ListRenderEndpoints())
    {
        if (endpoint.friendlyName.compare(0, namePrefix.size(), namePrefix) != 0)
            continue;
        if (found)
            return std::nullopt; // 複数あると決められない
        found = std::move(endpoint);
    }
    return found;
}
} // namespace pri::host
