// COM サーバーとしての入口（クラスファクトリー、登録・登録解除）

#include <windows.h>
#include <olectl.h>

#include <new>
#include <string>

#include "ApoGuids.h"
#include "ApoLog.h"
#include "Module.h"
#include "PriApo.h"

using namespace pri::apo;

namespace
{
HMODULE g_module = nullptr;

class ClassFactory final : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // 静的オブジェクトなので解放はしない。参照されている間は DLL をアンロードさせない
    STDMETHODIMP_(ULONG) AddRef() override
    {
        ++g_lockCount;
        return 2;
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        --g_lockCount;
        return 1;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        *ppv = nullptr;

        // 集約されるときは IUnknown しか要求できない（COM の決まり）
        if (outer != nullptr && riid != __uuidof(IUnknown))
            return CLASS_E_NOAGGREGATION;

        auto* apo = new (std::nothrow) PriApo(outer);
        if (apo == nullptr)
            return E_OUTOFMEMORY;

        const HRESULT hr = apo->NonDelegatingQueryInterface(riid, ppv);
        apo->NonDelegatingRelease();

        Log(L"CreateInstance: aggregated=%d hr=0x%08X", outer != nullptr ? 1 : 0, static_cast<unsigned>(hr));
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock)
            ++g_lockCount;
        else
            --g_lockCount;
        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring ClsidString()
{
    wchar_t text[40] = {};
    StringFromGUID2(kClsidPriSystemRackApo, text, _countof(text));
    return text;
}

std::wstring ClsidRegistryKey()
{
    return L"SOFTWARE\\Classes\\CLSID\\" + ClsidString();
}

HRESULT SetRegistryString(const std::wstring& subKey, const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, nullptr, 0,
                                     KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(status);

    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(status);
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* ppv)
{
    if (ppv == nullptr)
        return E_POINTER;
    *ppv = nullptr;

    if (rclsid != kClsidPriSystemRackApo)
        return CLASS_E_CLASSNOTAVAILABLE;

    return g_factory.QueryInterface(riid, ppv);
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow()
{
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

// regsvr32 から呼ばれる。COM クラスと APO を登録する（管理者権限が必要）
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return SELFREG_E_CLASS;

    const std::wstring key = ClsidRegistryKey();
    HRESULT hr = SetRegistryString(key, nullptr, kApoFriendlyName);
    if (SUCCEEDED(hr))
        hr = SetRegistryString(key + L"\\InprocServer32", nullptr, path);
    if (SUCCEEDED(hr))
        hr = SetRegistryString(key + L"\\InprocServer32", L"ThreadingModel", L"Both");
    if (FAILED(hr))
    {
        Log(L"DllRegisterServer: COM registration failed hr=0x%08X", static_cast<unsigned>(hr));
        return SELFREG_E_CLASS;
    }

    hr = RegisterAPO(PriApo::kRegProperties);
    Log(L"DllRegisterServer: path=%ls RegisterAPO hr=0x%08X", path, static_cast<unsigned>(hr));
    return SUCCEEDED(hr) ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer()
{
    const HRESULT apoHr = UnregisterAPO(kClsidPriSystemRackApo);
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidRegistryKey().c_str());
    Log(L"DllUnregisterServer: UnregisterAPO hr=0x%08X RegDeleteTree=%ld", static_cast<unsigned>(apoHr), status);

    // 片方だけ残っていた場合も含め、消えていれば成功とする
    return (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) ? S_OK : SELFREG_E_CLASS;
}
