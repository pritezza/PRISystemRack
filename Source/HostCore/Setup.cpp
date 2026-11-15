#include "Setup.h"

#include <windows.h>

#include <shellapi.h>

#include "Common/RackIpc.h"

namespace pri::host
{
bool IsApoRegistered()
{
    const std::wstring path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + ipc::kApoClsidString + L"\\InprocServer32";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

std::wstring FindSetupTool()
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};

    std::wstring tool(path, length);
    tool = tool.substr(0, tool.find_last_of(L'\\') + 1) + L"PRISystemRackSetup.exe";
    return GetFileAttributesW(tool.c_str()) != INVALID_FILE_ATTRIBUTES ? tool : std::wstring();
}

namespace
{
// HKLM\...\Policies\System の DWORD 値。なければ fallback
DWORD ReadUacPolicy(const wchar_t* name, DWORD fallback)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", name,
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}
} // namespace

bool WillShowUacPrompt()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return true;

    TOKEN_ELEVATION elevation = {};
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated != 0;
    GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size);
    CloseHandle(token);

    if (elevated)
        return false; // すでに管理者として動いている
    if (ReadUacPolicy(L"EnableLUA", 1) == 0)
        return false; // UAC そのものが切られている

    // 管理者のアカウント（権限を絞って動いている）で、「管理者には確認なしで昇格する」設定
    // （UAC のスライダーを一番下の「通知しない」にしたときなど）
    if (type == TokenElevationTypeLimited && ReadUacPolicy(L"ConsentPromptBehaviorAdmin", 5) == 0)
        return false;

    return true;
}

int RunSetupElevated(const std::wstring& arguments)
{
    const std::wstring tool = FindSetupTool();
    if (tool.empty())
        return -2;

    // ShellExecuteEx は COM を使うことがあるので、先に初期化しておく（ドキュメントの推奨）。呼び出し元は別スレッド
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComScope
    {
        bool initialised;
        ~ComScope()
        {
            if (initialised)
                CoUninitialize();
        }
    } comScope{ SUCCEEDED(comResult) };

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = tool.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info))
        return GetLastError() == ERROR_CANCELLED ? -1 : -2;

    if (info.hProcess == nullptr)
        return -2;

    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return static_cast<int>(exitCode);
}
} // namespace pri::host
