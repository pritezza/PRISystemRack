// PRISystemRackSetup: 再生デバイスに PRISystemRack APO を入れる・外す（管理者権限で実行する）。
//
//   PRISystemRackSetup.exe attach <エンドポイントの GUID> [--no-restart]
//       今の登録（Equalizer APO など）を HKLM\SOFTWARE\PRISystemRack\EndpointBackup\<GUID> に控えてから外し、
//       ミックス後の枠に PRISystemRack APO を登録する。
//   PRISystemRackSetup.exe detach <GUID> [--no-restart]
//       PRISystemRack APO を外し、控えておいた値をそのまま書き戻す。
//   PRISystemRackSetup.exe detach-all [--no-restart]
//       控えのあるすべてのデバイスを元に戻す。
//
// attach / detach には GUID を複数渡せる（本体は、選んだデバイスの後ろに、同じ出力のほかのサンプルレートのエンドポイントを並べる。
// AG06 などはレートごとに別のエンドポイントになるため）。先頭の結果が終了コードになり、2 つ目からの失敗はログに残すだけ。
//
// インストーラー（Installer/PRISystemRack.iss）から呼ぶ命令（どれも Windows Audio の再起動はしない。uninstall だけは最後に再起動する）:
//   PRISystemRackSetup.exe quit-app [<本体の exe>]
//       動いている本体に --quit を送り、終わるのを待つ（ファイルを置き換えられるように）。--quit を送るのに起動するのは、
//       渡した exe（インストーラーは {app}\PRISystemRack.exe）か、なければこの exe と同じフォルダーの本体だけ
//       （管理者として動くので、動いているプロセスの exe（どこに置かれたものか分からない）は起動しない）。
//   PRISystemRackSetup.exe stop-audio / start-audio
//       Windows Audio を止める（audiodg.exe が終わるまで待つ。APO の DLL を置き換えるため）/ 動かす。
//   PRISystemRackSetup.exe register <APO の DLL のパス>
//       ログフォルダーを作って LOCAL SERVICE に書き込みを許可し、署名のない APO を読み込めるようにし（DisableProtectedAudioDG）、
//       DLL の DllRegisterServer を呼ぶ（COM と APO の登録）。
//   PRISystemRackSetup.exe uninstall
//       控えのあるすべてのデバイスを元に戻し（detach-all）、同じフォルダーの APO の DLL の DllUnregisterServer を呼び、
//       HKLM\SOFTWARE\PRISystemRack を消して、Windows Audio を再起動する（DLL を消せるように）。
//
// attach / detach / detach-all は、最後に Windows Audio サービスを再起動して反映する（--no-restart で省く）。
// 終了コード: 0 = 成功、1 = 失敗、2 = 使い方の誤り、3 = 選んだデバイスは切り替えた（反映も済み）が、2 つ目からのエンドポイントの
// どれかで失敗した（本体はステータスバーに注意として出す）、4 = 選んだデバイスがレジストリの再生デバイスにない（リモート デスクトップの
// 「リモート オーディオ」など。APO を入れられない）。結果は %ProgramData%\PRISystemRack\Logs\setup.log にも書く。

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "Common/FxSlots.h"
#include "Common/RackIpc.h"

namespace
{
constexpr wchar_t kRenderRoot[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
constexpr const wchar_t* kBackupRoot = pri::fx::kBackupRoot;
constexpr const wchar_t* kPostMixValue = pri::fx::kPostMixValue;

// 控えて外す値: APO の CLSID が入る枠すべて（本体アプリの判定と同じ一覧）と、設定画面に出る名前
std::vector<const wchar_t*> FxValueNames()
{
    std::vector<const wchar_t*> names(std::begin(pri::fx::kApoSlotValues), std::end(pri::fx::kApoSlotValues));
    names.push_back(pri::fx::kNameValue);
    return names;
}

// 控えに一緒に入れる、元の値ではない情報
constexpr wchar_t kMetaCreated[] = L"FxPropertiesCreated";
constexpr wchar_t kMetaName[] = L"EndpointName";

HANDLE g_log = INVALID_HANDLE_VALUE; // setup.log（UTF-8 で追記する）
bool g_restorePrivilege = false;     // SeRestorePrivilege を有効にできた（アクセス権に関係なくキーを作る・消すのに使う）

void Print(const wchar_t* format, ...)
{
    wchar_t text[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args);
    va_end(args);

    std::wprintf(L"%ls\n", text);
    if (g_log != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t line[1100];
        swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u %ls\r\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                   now.wSecond, text);
        char utf8[4096];
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        DWORD written = 0;
        if (bytes > 1)
            WriteFile(g_log, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
}

void CloseLog()
{
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
}

std::wstring ProgramDataFolder()
{
    wchar_t programData[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(programData) : std::wstring();
}

// 管理者として使ってよいフォルダーにする。一般ユーザーも C:\ProgramData の下にはフォルダーを作れるので、インストールの前に
// 作られていたもの（とくに別の場所を指すジャンクション）をそのまま使うと、管理者の権限で別の場所に書いたり権限を付けたりしてしまう。
// - リンク（ジャンクションなど）なら、リンクそのものを消して（指している先は消さない）作り直す
// - 持ち主を Administrators にし、明示の権限を explicitAccess だけに置き換える（継承は親から付け直す。先に作った一般ユーザーの
//   権限は消えるので、一般ユーザーはこのフォルダーを消したり置き換えたりできなくなる）
bool SecureFolder(const std::wstring& path, const EXPLICIT_ACCESSW* explicitAccess)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        if (!RemoveDirectoryW(path.c_str()) && !DeleteFileW(path.c_str()))
        {
            Print(L"error: %ls is a link and cannot be removed (%lu)", path.c_str(), GetLastError());
            return false;
        }
        Print(L"warning: removed the link %ls (it did not point to a folder created by setup)", path.c_str());
        attributes = INVALID_FILE_ATTRIBUTES;
    }
    if (attributes == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        Print(L"error: cannot create %ls (%lu)", path.c_str(), GetLastError());
        return false;
    }
    attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        Print(L"error: %ls is not a plain folder", path.c_str());
        return false;
    }

    PSID administrators = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-32-544", &administrators))
        return false;

    PACL acl = nullptr;
    DWORD status = ERROR_SUCCESS;
    if (explicitAccess != nullptr)
    {
        status = SetEntriesInAclW(1, const_cast<EXPLICIT_ACCESSW*>(explicitAccess), nullptr, &acl);
    }
    else if ((acl = static_cast<PACL>(LocalAlloc(LPTR, sizeof(ACL)))) == nullptr || !InitializeAcl(acl, sizeof(ACL), ACL_REVISION))
    {
        status = ERROR_NOT_ENOUGH_MEMORY;
    }

    if (status == ERROR_SUCCESS)
        status = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                       administrators, nullptr, acl, nullptr);

    if (acl != nullptr)
        LocalFree(acl);
    LocalFree(administrators);

    if (status != ERROR_SUCCESS)
    {
        Print(L"warning: cannot secure %ls (%lu)", path.c_str(), status);
        return false;
    }
    return true;
}

// %ProgramData%\PRISystemRack と、その下の Logs を用意する。Logs には audiodg.exe（LOCAL SERVICE）が apo.log を書けるよう
// 変更の権限を付ける（下のファイル・フォルダーにも継承）。作れなければ空
std::wstring PrepareLogFolders()
{
    const auto programData = ProgramDataFolder();
    if (programData.empty())
        return {};
    const std::wstring root = programData + L"\\PRISystemRack";
    const std::wstring logs = root + L"\\Logs";
    if (!SecureFolder(root, nullptr))
        return {};

    PSID localService = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-19", &localService))
        return {};
    EXPLICIT_ACCESSW access = {};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE; // icacls の M（変更）
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(localService);
    const bool secured = SecureFolder(logs, &access);
    LocalFree(localService);
    return secured ? logs : std::wstring();
}

// setup.log を開く。一般ユーザーが先に置いたリンク（シンボリックリンク・ハードリンク）なら、リンクの名前だけを消してから作り直す
// （そのまま開くと、管理者として別のファイルに追記してしまう）
HANDLE OpenLogFile(const std::wstring& path)
{
    const auto open = [&path](DWORD disposition)
    {
        return CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    };

    HANDLE file = open(OPEN_ALWAYS);
    if (file == INVALID_HANDLE_VALUE)
        return file;

    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileInformationByHandle(file, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && info.nNumberOfLinks == 1)
        return file;

    CloseHandle(file);
    if (!DeleteFileW(path.c_str()))
        return INVALID_HANDLE_VALUE;
    return open(CREATE_NEW);
}

void OpenLog()
{
    const auto dir = PrepareLogFolders();
    if (dir.empty())
        return;

    // 1 MB を超えていたら setup.old.log に回して新しく書き始める（ファイルは最大 2 つ）。リンクだった場合も、動くのはリンクそのもの
    const std::wstring path = dir + L"\\setup.log";
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)
        && ((static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) >= 1024 * 1024)
        MoveFileExW(path.c_str(), (dir + L"\\setup.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);

    g_log = OpenLogFile(path);
}

struct RegValue
{
    DWORD type = 0;
    std::vector<BYTE> data;
};

bool ReadValue(HKEY key, const wchar_t* name, RegValue& value)
{
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &value.type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    value.data.resize(size);
    return RegQueryValueExW(key, name, nullptr, &value.type, value.data.data(), &size) == ERROR_SUCCESS;
}

std::wstring ValueText(const RegValue& value)
{
    // レジストリの文字列は終端の 0 がないことがあるので、データの長さを超えて読まない
    if ((value.type == REG_SZ || value.type == REG_MULTI_SZ) && value.data.size() >= sizeof(wchar_t))
    {
        const auto* text = reinterpret_cast<const wchar_t*>(value.data.data());
        return std::wstring(text, wcsnlen(text, value.data.size() / sizeof(wchar_t)));
    }
    return L"(binary)";
}

class Key
{
public:
    Key() = default;
    ~Key() { Close(); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;

    LSTATUS Open(const std::wstring& path, REGSAM access)
    {
        Close();
        return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, access | KEY_WOW64_64KEY, &key_);
    }

    LSTATUS Create(const std::wstring& path, REGSAM access, bool* created = nullptr)
    {
        Close();
        DWORD disposition = 0;
        LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, access | KEY_WOW64_64KEY,
                                         nullptr, &key_, &disposition);

        // 再生デバイスのキー（MMDevices\Audio\Render\{GUID}）は、Windows の初期の権限のままだと、管理者でも値は書けるが下にキーを作れない
        // （AG06 で初めて使ったサンプルレートのエンドポイントなど、FxProperties がまだないもの）。
        // そのときは「復元」の特権でアクセス権に関係なく作る（権限は書き換えない。作ったキーは親の権限を受け継ぐので、あとは普通に開いて書ける）
        if (status == ERROR_ACCESS_DENIED && g_restorePrivilege)
        {
            status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_BACKUP_RESTORE,
                                     access | KEY_WOW64_64KEY, nullptr, &key_, &disposition);
            if (status == ERROR_SUCCESS)
                Print(L"note: used the restore privilege to open or create %ls (the parent key does not allow creating subkeys)", path.c_str());
        }

        if (created != nullptr)
            *created = disposition == REG_CREATED_NEW_KEY;
        return status;
    }

    void Close()
    {
        if (key_ != nullptr)
        {
            RegCloseKey(key_);
            key_ = nullptr;
        }
    }

    HKEY Get() const { return key_; }
    explicit operator bool() const { return key_ != nullptr; }

private:
    HKEY key_ = nullptr;
};

bool KeyExists(const std::wstring& path)
{
    Key key;
    return key.Open(path, KEY_READ) == ERROR_SUCCESS;
}

// 管理者のトークンにある特権を有効にする（既定では無効になっている）
bool EnablePrivilege(const wchar_t* name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)
                    && AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)
                    && GetLastError() == ERROR_SUCCESS; // 持っていない特権は ERROR_NOT_ALL_ASSIGNED
    CloseHandle(token);
    return ok;
}

// キーを消す（中は空のもの）。アクセス権で消せなければ、「復元」の特権で開いて消す（Key::Create が特権で作った FxProperties は、
// 親の権限を受け継いでいて管理者に削除の権限がない）
LSTATUS DeleteKey(const std::wstring& path)
{
    LSTATUS status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), KEY_WOW64_64KEY, 0);
    if (status != ERROR_ACCESS_DENIED || !g_restorePrivilege)
        return status;

    using NtDeleteKeyFunction = LONG(NTAPI*)(HANDLE);
    const auto ntDeleteKey = reinterpret_cast<NtDeleteKeyFunction>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeleteKey")));
    if (ntDeleteKey == nullptr)
        return status;

    // 「復元」の特権で開くと DELETE の権限が付く（すでにあるので作られない）
    HKEY key = nullptr;
    DWORD disposition = 0;
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_WOW64_64KEY, nullptr,
                             &key, &disposition);
    if (status != ERROR_SUCCESS)
        return status;

    const LONG ntStatus = ntDeleteKey(key);
    RegCloseKey(key);
    return ntStatus == 0 ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

bool IsApoRegistered()
{
    return KeyExists(std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + pri::ipc::kApoClsidString + L"\\InprocServer32");
}

std::wstring EndpointName(const std::wstring& guid)
{
    Key key;
    if (key.Open(kRenderRoot + guid + L"\\Properties", KEY_READ) != ERROR_SUCCESS)
        return {};
    RegValue value;
    if (!ReadValue(key.Get(), L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", value))
        return {};
    return ValueText(value);
}

int Fail(const wchar_t* what, LSTATUS status)
{
    Print(L"error: %ls (%ld)", what, status);
    return 1;
}

//==============================================================================
int Attach(const std::wstring& guid)
{
    if (!IsApoRegistered())
    {
        Print(L"error: the PRISystemRack APO is not registered (install the APO first)");
        return 1;
    }

    const std::wstring endpointPath = kRenderRoot + guid;
    if (!KeyExists(endpointPath))
    {
        // リモート デスクトップの「リモート オーディオ」などのセッションごとの仮想のデバイスは、ここに載らない（APO を入れられない）
        Print(L"error: playback device %ls was not found in the registry (a Remote Desktop audio device, for example, cannot have an APO)",
              guid.c_str());
        return 4;
    }

    const std::wstring fxPath = endpointPath + L"\\FxProperties";
    const std::wstring backupPath = std::wstring(kBackupRoot) + L"\\" + guid;
    const std::wstring name = EndpointName(guid);

    // ミックス後の枠に PRISystemRack APO を登録する（失敗したら what に理由）
    const wchar_t* what = nullptr;
    auto registerApo = [&]() -> LSTATUS
    {
        Key fx;
        if (const LSTATUS status = fx.Create(fxPath, KEY_SET_VALUE); status != ERROR_SUCCESS)
        {
            what = L"cannot create FxProperties";
            return status;
        }

        const std::wstring clsid = pri::ipc::kApoClsidString;
        const LSTATUS status = RegSetValueExW(fx.Get(), kPostMixValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(clsid.c_str()),
                                              static_cast<DWORD>((clsid.size() + 1) * sizeof(wchar_t)));
        if (status != ERROR_SUCCESS)
            what = L"cannot register the APO";
        return status;
    };

    if (KeyExists(backupPath))
    {
        Print(L"%ls %ls: a backup already exists (PRISystemRack is attached); only checking the registration", name.c_str(), guid.c_str());
        if (const LSTATUS status = registerApo(); status != ERROR_SUCCESS)
            return Fail(what, status);

        Print(L"attached: %ls %ls", name.c_str(), guid.c_str());
        return 0;
    }

    const bool fxExisted = KeyExists(fxPath);

    // 控えを作る前に、FxProperties を書き込める形で開いておく（開けないまま控えを作ると、中身のない控えが残り、
    // 次の attach が「控えあり」とみなして今の登録を控えずに上書きしてしまう）
    Key fx;
    if (fxExisted)
    {
        if (const LSTATUS status = fx.Open(fxPath, KEY_QUERY_VALUE | KEY_SET_VALUE); status != ERROR_SUCCESS)
            return Fail(L"cannot open FxProperties (no write permission?)", status);
    }

    Key backup;
    if (const LSTATUS status = backup.Create(backupPath, KEY_SET_VALUE); status != ERROR_SUCCESS)
        return Fail(L"cannot create the backup key", status);

    // 途中で失敗したら、外した値を書き戻して控えを消す（中身の欠けた控えを残さない）
    std::vector<std::pair<const wchar_t*, RegValue>> removed;
    auto rollback = [&](const wchar_t* failure, LSTATUS failureStatus) -> int
    {
        for (const auto& [valueName, value] : removed)
            RegSetValueExW(fx.Get(), valueName, 0, value.type, value.data.data(), static_cast<DWORD>(value.data.size()));

        if (!fxExisted)
        {
            fx.Close();
            if (const LSTATUS status = DeleteKey(fxPath); status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                Print(L"warning: could not remove the FxProperties key created here (%ld); it is empty and harmless", status);
        }

        backup.Close();
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupPath.c_str());
        Print(L"rolled back: restored %zu value(s) and removed the backup", removed.size());
        return Fail(failure, failureStatus);
    };

    const DWORD created = fxExisted ? 0 : 1;
    if (const LSTATUS status = RegSetValueExW(backup.Get(), kMetaCreated, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&created), sizeof(created));
        status != ERROR_SUCCESS)
        return rollback(L"cannot write the backup", status);
    if (const LSTATUS status = RegSetValueExW(backup.Get(), kMetaName, 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                                              static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
        status != ERROR_SUCCESS)
        return rollback(L"cannot write the backup", status);

    // 今の値を控えてから外す
    if (fxExisted)
    {
        for (const wchar_t* valueName : FxValueNames())
        {
            RegValue value;
            if (!ReadValue(fx.Get(), valueName, value))
                continue;

            if (const LSTATUS status = RegSetValueExW(backup.Get(), valueName, 0, value.type, value.data.data(),
                                                      static_cast<DWORD>(value.data.size()));
                status != ERROR_SUCCESS)
                return rollback(L"cannot back up a value", status);

            if (const LSTATUS status = RegDeleteValueW(fx.Get(), valueName); status != ERROR_SUCCESS)
                return rollback(L"cannot remove a value", status);

            removed.emplace_back(valueName, value);
            Print(L"backed up and removed: %ls = %ls", valueName, ValueText(value).c_str());
        }
    }

    if (const LSTATUS status = registerApo(); status != ERROR_SUCCESS)
        return rollback(what, status);

    Print(L"attached: %ls %ls", name.c_str(), guid.c_str());
    return 0;
}

int Detach(const std::wstring& guid)
{
    const std::wstring fxPath = kRenderRoot + guid + L"\\FxProperties";
    const std::wstring backupPath = std::wstring(kBackupRoot) + L"\\" + guid;
    const std::wstring name = EndpointName(guid);

    // デバイスの登録そのものがもうない（ドライバーごと消えたなど）: 書き戻す先がないので、控えを消して終わる
    // （書き戻そうとすると、途中のキーまで作って偽のデバイスのキーができるか、毎回失敗して控えが残り続ける）
    // 「ない」と分かったときだけにする（権限などの一時的な失敗で控えを消さないよう、開けなかった理由を見る）
    Key endpointKey;
    if (const LSTATUS status = endpointKey.Open(kRenderRoot + guid, KEY_READ); status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return Fail((L"cannot open playback device " + guid).c_str(), status);
    if (!endpointKey)
    {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupPath.c_str());
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), KEY_WOW64_64KEY, 0);
        Print(L"warning: playback device %ls no longer exists, so its backup was discarded without restoring", guid.c_str());
        return 0;
    }

    Key fx;
    const bool fxExists = fx.Open(fxPath, KEY_QUERY_VALUE | KEY_SET_VALUE) == ERROR_SUCCESS;

    // PRISystemRack の値を外す（別の値に変わっていたら触らない）
    if (fxExists)
    {
        RegValue current;
        if (ReadValue(fx.Get(), kPostMixValue, current))
        {
            if (_wcsicmp(ValueText(current).c_str(), pri::ipc::kApoClsidString) == 0)
                RegDeleteValueW(fx.Get(), kPostMixValue);
            else
                Print(L"warning: the post-mix slot holds another APO (%ls); leaving it", ValueText(current).c_str());
        }
    }

    Key backup;
    if (backup.Open(backupPath, KEY_QUERY_VALUE) != ERROR_SUCCESS)
    {
        Print(L"%ls %ls: no backup; removed only the PRISystemRack value", name.c_str(), guid.c_str());
        return 0;
    }

    // 控えた値をそのまま書き戻す
    DWORD created = 0;
    {
        RegValue meta;
        if (ReadValue(backup.Get(), kMetaCreated, meta) && meta.type == REG_DWORD && meta.data.size() == sizeof(DWORD))
            created = *reinterpret_cast<const DWORD*>(meta.data.data());
    }

    // attach の後に別の APO が入れ直されていたら（今その値に別の中身がある）、新しいほうを残して控えの値は書き戻さない。
    // 書き戻さなかった値はログに残す（控えは消す。残すと次の attach が「控えあり」とみなして今の登録を控えずに上書きするため）
    int kept = 0;
    for (const wchar_t* valueName : FxValueNames())
    {
        RegValue value;
        if (!ReadValue(backup.Get(), valueName, value))
            continue;

        if (!fx)
        {
            if (const LSTATUS status = fx.Create(fxPath, KEY_QUERY_VALUE | KEY_SET_VALUE); status != ERROR_SUCCESS)
                return Fail(L"cannot create FxProperties", status);
        }

        RegValue current;
        if (ReadValue(fx.Get(), valueName, current))
        {
            if (current.type != value.type || current.data != value.data)
            {
                Print(L"warning: kept the current value %ls = %ls (another APO was installed after attach); not restored: %ls",
                      valueName, ValueText(current).c_str(), ValueText(value).c_str());
                ++kept;
            }
            continue;
        }

        if (const LSTATUS status = RegSetValueExW(fx.Get(), valueName, 0, value.type, value.data.data(),
                                                  static_cast<DWORD>(value.data.size()));
            status != ERROR_SUCCESS)
            return Fail(L"cannot restore a value", status);

        Print(L"restored: %ls = %ls", valueName, ValueText(value).c_str());
    }

    if (kept > 0)
        Print(L"warning: %d saved value(s) were not restored because the device now has other values (see the lines above)", kept);

    // FxProperties を PRISystemRack が作ったもので、空になっていれば消す
    if (created == 1 && fx)
    {
        DWORD valueCount = 0;
        DWORD subKeyCount = 0;
        RegQueryInfoKeyW(fx.Get(), nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr, &valueCount, nullptr,
                         nullptr, nullptr, nullptr);
        fx.Close();
        if (valueCount == 0 && subKeyCount == 0)
            if (const LSTATUS status = DeleteKey(fxPath); status != ERROR_SUCCESS)
                Print(L"warning: could not remove the empty FxProperties key that attach created (%ld); it is harmless", status);
    }

    backup.Close();
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupPath.c_str());

    Print(L"detached and restored: %ls %ls", name.c_str(), guid.c_str());
    return 0;
}

int DetachAll()
{
    std::vector<std::wstring> guids;
    {
        Key root;
        if (root.Open(kBackupRoot, KEY_READ) != ERROR_SUCCESS)
        {
            Print(L"no backups");
            return 0;
        }

        for (DWORD index = 0;; ++index)
        {
            wchar_t subKey[256];
            DWORD length = _countof(subKey);
            if (RegEnumKeyExW(root.Get(), index, subKey, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            guids.emplace_back(subKey);
        }
    }

    int result = 0;
    for (const auto& guid : guids)
        if (Detach(guid) != 0)
            result = 1;
    return result;
}

//==============================================================================
bool WaitForServiceState(SC_HANDLE service, DWORD state, DWORD timeoutMs)
{
    const ULONGLONG start = GetTickCount64();
    SERVICE_STATUS status = {};
    while (QueryServiceStatus(service, &status))
    {
        if (status.dwCurrentState == state)
            return true;
        if (GetTickCount64() - start > timeoutMs)
            return false;
        Sleep(100);
    }
    return false;
}

// Windows Audio（Audiosrv）のサービスを開いておく
struct AudioService
{
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;

    AudioService()
    {
        manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (manager == nullptr)
            Print(L"error: cannot open the service manager (%lu)", GetLastError());
        else if ((service = OpenServiceW(manager, L"Audiosrv",
                                         SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS)) == nullptr)
            Print(L"error: cannot open the Windows Audio service (%lu)", GetLastError());
    }
    ~AudioService()
    {
        if (service != nullptr)
            CloseServiceHandle(service);
        if (manager != nullptr)
            CloseServiceHandle(manager);
    }
};

// 名前が name のプロセスの PID
std::vector<DWORD> FindProcesses(const wchar_t* name)
{
    std::vector<DWORD> found;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return found;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, name) == 0)
            found.push_back(entry.th32ProcessID);
    CloseHandle(snapshot);
    return found;
}

// Audiosrv に依存しているサービス（state: SERVICE_ACTIVE / SERVICE_INACTIVE）の名前。止める順（依存の深いものから）に並ぶ
std::vector<std::wstring> DependentServices(AudioService& audio, DWORD state)
{
    std::vector<std::wstring> names;
    DWORD needed = 0, count = 0;
    if (EnumDependentServicesW(audio.service, state, nullptr, 0, &needed, &count) || GetLastError() != ERROR_MORE_DATA || needed == 0)
        return names; // ない
    std::vector<BYTE> buffer(needed);
    auto* list = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buffer.data());
    if (EnumDependentServicesW(audio.service, state, list, needed, &needed, &count))
        for (DWORD i = 0; i < count; ++i)
            names.emplace_back(list[i].lpServiceName);
    return names;
}

// StopAudio が止めた依存するサービスの名前を控える場所（stop-audio と start-audio は別のプロセスなので、レジストリに置く）
constexpr wchar_t kStateKey[] = L"SOFTWARE\\PRISystemRack";
constexpr wchar_t kStoppedServicesValue[] = L"StoppedServices";

void RememberStoppedServices(const std::vector<std::wstring>& names)
{
    if (names.empty())
        return;
    std::wstring multi;
    for (const auto& name : names)
        multi += name + L'\0';
    multi += L'\0';
    Key key;
    if (key.Create(kStateKey, KEY_SET_VALUE) == ERROR_SUCCESS)
        RegSetValueExW(key.Get(), kStoppedServicesValue, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()),
                       static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
}

// 控えた名前を読み、控えを消す（控えのためだけに作ったキーが空になったら、キーも消す）
std::vector<std::wstring> TakeStoppedServices()
{
    std::vector<std::wstring> names;
    Key key;
    if (key.Open(kStateKey, KEY_QUERY_VALUE | KEY_SET_VALUE) != ERROR_SUCCESS)
        return names;

    RegValue value;
    if (ReadValue(key.Get(), kStoppedServicesValue, value) && value.type == REG_MULTI_SZ)
    {
        const auto* text = reinterpret_cast<const wchar_t*>(value.data.data());
        const size_t count = value.data.size() / sizeof(wchar_t);
        for (size_t i = 0; i < count && text[i] != 0;)
        {
            const size_t length = wcsnlen(text + i, count - i);
            names.emplace_back(text + i, length);
            i += length + 1;
        }
    }
    RegDeleteValueW(key.Get(), kStoppedServicesValue);

    DWORD subKeys = 0, values = 0;
    RegQueryInfoKeyW(key.Get(), nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    key.Close();
    if (subKeys == 0 && values == 0)
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kStateKey, KEY_WOW64_64KEY, 0);
    return names;
}

// 止めて、APO を読み込んでいる audiodg.exe が終わるまで待つ（止まらなければ false）。
// Audiosrv に依存するサービス（機器のメーカーの音声サービスなど）が動いていると止められない（ERROR_DEPENDENT_SERVICES_RUNNING）ので、
// 先にそれらを止め、止めたものの名前を控える（StartAudio が、それだけを動かし直す。もともと止まっていたものは動かさない）
bool StopAudio(AudioService& audio)
{
    std::vector<std::wstring> stopped;
    for (const auto& name : DependentServices(audio, SERVICE_ACTIVE))
        if (const SC_HANDLE dependent = OpenServiceW(audio.manager, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS))
        {
            SERVICE_STATUS status = {};
            if (ControlService(dependent, SERVICE_CONTROL_STOP, &status) && WaitForServiceState(dependent, SERVICE_STOPPED, 15000))
            {
                Print(L"stopped the dependent service %ls", name.c_str());
                stopped.push_back(name);
            }
            else
            {
                Print(L"warning: cannot stop the dependent service %ls (%lu)", name.c_str(), GetLastError());
            }
            CloseServiceHandle(dependent);
        }
    RememberStoppedServices(stopped);

    SERVICE_STATUS status = {};
    if (!ControlService(audio.service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
        Print(L"warning: the stop request failed (%lu%ls)", GetLastError(),
              GetLastError() == ERROR_DEPENDENT_SERVICES_RUNNING ? L": another service that depends on Windows Audio is running" : L"");

    if (!WaitForServiceState(audio.service, SERVICE_STOPPED, 15000))
        return false;

    // audiodg.exe が残っていると APO の DLL を使ったままなので、置き換えられない（失敗にする）
    const ULONGLONG start = GetTickCount64();
    while (!FindProcesses(L"audiodg.exe").empty())
    {
        if (GetTickCount64() - start > 15000)
        {
            Print(L"error: audiodg.exe is still running after Windows Audio stopped");
            return false;
        }
        Sleep(100);
    }
    return true;
}

int StartAudio(AudioService& audio)
{
    // 依存している AudioEndpointBuilder が止まっていれば、サービスの管理が一緒に起動する
    if (!StartServiceW(audio.service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        return Fail(L"cannot start Windows Audio", static_cast<LSTATUS>(GetLastError()));
    if (!WaitForServiceState(audio.service, SERVICE_RUNNING, 15000))
        return Fail(L"Windows Audio does not start", 0);

    // StopAudio が止めた依存するサービスだけを動かし直す（待たない。もともと止まっていたものは動かさない）
    for (const auto& name : TakeStoppedServices())
        if (const SC_HANDLE dependent = OpenServiceW(audio.manager, name.c_str(), SERVICE_START))
        {
            if (StartServiceW(dependent, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                Print(L"started the dependent service %ls", name.c_str());
            else
                Print(L"warning: cannot start the dependent service %ls (%lu)", name.c_str(), GetLastError());
            CloseServiceHandle(dependent);
        }
    return 0;
}

// Windows Audio を再起動して、APO の登録の変更を読み込ませる
int RestartAudioService()
{
    AudioService audio;
    if (audio.service == nullptr)
        return 1;

    Print(L"restarting Windows Audio (sound stops for a few seconds)");

    // 止まらなかったときは、登録の変更がまだ読み込まれていない。音が止まったままにならないよう起動は試み、失敗を返す
    const bool stopped = StopAudio(audio);
    if (!stopped)
        Print(L"error: Windows Audio did not stop, so the change is not applied yet (restart the PC to apply it)");

    const int result = StartAudio(audio);
    return result == 0 && !stopped ? 1 : result;
}

//==============================================================================
// インストーラーから呼ぶ命令

int StopAudioCommand()
{
    AudioService audio;
    if (audio.service == nullptr)
        return 1;
    Print(L"stopping Windows Audio to replace the APO (sound stops until the installation finishes)");
    if (!StopAudio(audio))
        return Fail(L"Windows Audio did not stop", 0);
    return 0;
}

int StartAudioCommand()
{
    AudioService audio;
    if (audio.service == nullptr)
        return 1;
    const int result = StartAudio(audio);
    if (result == 0)
        Print(L"Windows Audio is running");
    return result;
}

std::wstring FolderOfPath(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// 動いている本体に --quit を送り、本体と補助プログラム（Scanner）が終わるのを待つ。
// trustedExe: 終了を頼むのに起動してよい本体の exe（インストーラーは {app}\PRISystemRack.exe を渡す。省略時はこの exe と同じフォルダーのもの）。
// 管理者として動いているので、動いているプロセスの exe（どこにあるか分からず、一般ユーザーが置いたものかもしれない）は起動しない。
// --quit は同じアプリのどの exe から送っても、動いている本体（1 つだけ）に届く
int QuitApp(std::wstring trustedExe)
{
    if (trustedExe.empty())
    {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        trustedExe = FolderOfPath(self) + L"\\PRISystemRack.exe";
    }
    const std::wstring trustedFolder = FolderOfPath(trustedExe);

    std::vector<HANDLE> processes;
    for (const wchar_t* name : { L"PRISystemRack.exe", L"PRISystemRackScanner.exe" })
        for (const DWORD pid : FindProcesses(name))
            if (const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid))
                processes.push_back(process);

    if (processes.empty())
    {
        Print(L"the application is not running");
        return 0;
    }

    // 本体の終了は、信頼できる場所の exe を --quit 付きで起動して頼む（× や WM_CLOSE では通知領域にしまうだけなので）。
    // まだ入っていない（初めてのインストールで、ほかの場所の本体が動いている）ときは頼めないので、終わるのを待つだけ
    const bool canAsk = GetFileAttributesW(trustedExe.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!canAsk)
    {
        Print(L"warning: %ls does not exist, so the running application cannot be asked to quit", trustedExe.c_str());
    }
    else
    {
        const std::wstring appPath = trustedExe;
        Print(L"asking the application to quit: %ls", appPath.c_str());
        std::wstring commandLine = L"\"" + appPath + L"\" --quit";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info = {};
        if (CreateProcessW(appPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
        {
            WaitForSingleObject(info.hProcess, 10000);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
        else
        {
            Print(L"warning: cannot start %ls --quit (%lu)", appPath.c_str(), GetLastError());
        }
    }

    // 本体は設定を保存してプラグインを解放してから終わるので、少し長めに待つ（頼めなかったときは、たまたま終わる場合だけ少し待つ）
    const ULONGLONG timeoutMs = canAsk ? 30000 : 2000;
    int result = 0;
    const ULONGLONG start = GetTickCount64();
    for (const HANDLE process : processes)
    {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (WaitForSingleObject(process, elapsed < timeoutMs ? static_cast<DWORD>(timeoutMs - elapsed) : 0) != WAIT_OBJECT_0)
        {
            wchar_t path[MAX_PATH] = L"?";
            DWORD length = MAX_PATH;
            QueryFullProcessImageNameW(process, 0, path, &length);
            // 補助プログラム（プラグインを調べているだけ）は止めてよい。ただし信頼できる場所のものだけ（ほかの人のプロセスは止めない）
            const bool helper = _wcsicmp(path, (trustedFolder + L"\\PRISystemRackScanner.exe").c_str()) == 0;
            if (helper && TerminateProcess(process, 1))
                Print(L"stopped the helper %ls", path);
            else
                result = Fail(L"the application did not quit", 0);
        }
        CloseHandle(process);
    }
    if (result == 0)
        Print(L"the application has quit");
    return result;
}

// ログフォルダーを用意したことを記録する（作るのと権限を付けるのは、起動の最初の OpenLog → PrepareLogFolders で済ませてある）
bool PrepareLogFolder()
{
    const auto dir = PrepareLogFolders();
    if (dir.empty())
    {
        Print(L"warning: the log folder could not be prepared, so the APO may not be able to write apo.log");
        return false;
    }
    Print(L"log folder ready: %ls", dir.c_str());
    return true;
}

// 署名のない APO を audiodg.exe に読み込ませる（HKLM\...\Audio の DisableProtectedAudioDG = 1）。
// ほかの APO（署名のないもの）も頼っていることがあるので、アンインストールでは戻さない
void AllowUnsignedApos()
{
    Key key;
    if (key.Create(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", KEY_READ | KEY_SET_VALUE) != ERROR_SUCCESS)
    {
        Print(L"warning: cannot open the Audio settings key");
        return;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key.Get(), L"DisableProtectedAudioDG", nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS
        && value == 1)
    {
        Print(L"DisableProtectedAudioDG is already 1");
        return;
    }
    value = 1;
    if (RegSetValueExW(key.Get(), L"DisableProtectedAudioDG", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS)
        Print(L"set DisableProtectedAudioDG = 1 (so that Windows Audio loads the unsigned APO)");
    else
        Print(L"warning: cannot set DisableProtectedAudioDG");
}

// APO の DLL の DllRegisterServer / DllUnregisterServer を呼ぶ（regsvr32 と同じ）
int CallDllFunction(const std::wstring& dll, const char* name)
{
    const HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr)
        return Fail((L"cannot load " + dll).c_str(), static_cast<LSTATUS>(GetLastError()));

    using Function = HRESULT(STDAPICALLTYPE*)();
    const auto function = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    const HRESULT hr = function != nullptr ? function() : E_NOTIMPL;
    FreeLibrary(module);

    if (FAILED(hr))
    {
        Print(L"error: %hs failed (hr=0x%08X): %ls", name, static_cast<unsigned>(hr), dll.c_str());
        return 1;
    }
    Print(L"%hs: %ls", name, dll.c_str());
    return 0;
}

int Register(const std::wstring& dll)
{
    PrepareLogFolder(); // DllRegisterServer も apo.log に書くので先に
    AllowUnsignedApos();
    return CallDllFunction(dll, "DllRegisterServer");
}

int Uninstall()
{
    // 1. PRISystemRack を入れたデバイスをすべて元に戻す（控えておいた Equalizer APO などの登録を書き戻す）
    const int detached = DetachAll();

    // 2. APO の登録を消す（この exe と同じフォルダーの DLL）
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dll(self);
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"PRISystemRackAPO.dll";
    int result = GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES ? CallDllFunction(dll, "DllUnregisterServer")
                                                                              : (Print(L"warning: %ls was not found", dll.c_str()), 0);

    // 3. 控えなどのキーを消す（元に戻せなかったデバイスがあれば、控えを残す）
    if (detached == 0)
    {
        LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\PRISystemRack");
        if (status == ERROR_SUCCESS)
            status = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\PRISystemRack", KEY_WOW64_64KEY, 0);
        if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
            Print(L"removed HKLM\\SOFTWARE\\PRISystemRack");
        else
            Print(L"warning: cannot remove HKLM\\SOFTWARE\\PRISystemRack (%ld)", status);
    }
    else
    {
        Print(L"error: some devices could not be restored, so their backups are kept in HKLM\\%ls", kBackupRoot);
        result = 1;
    }

    // 4. Windows Audio を再起動する（元に戻した登録を反映し、APO の DLL を手放させて消せるようにする）
    if (RestartAudioService() != 0)
        result = 1;
    return result;
}

bool LooksLikeGuid(const std::wstring& text)
{
    return text.size() == 38 && text.front() == L'{' && text.back() == L'}';
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");
    OpenLog();

    // FxProperties がまだないエンドポイントでは、キーを作るのに「復元」の特権が要ることがある（Key::Create）。読むために「バックアップ」も
    g_restorePrivilege = EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_BACKUP_NAME);

    std::vector<std::wstring> args(argv + 1, argv + argc);
    bool restart = true;
    for (auto it = args.begin(); it != args.end();)
    {
        if (*it == L"--no-restart")
        {
            restart = false;
            it = args.erase(it);
        }
        else
            ++it;
    }

    // attach / detach の GUID は複数並べてよい。先頭が選んだデバイスで、その結果を終了コードにする。
    // 2 つ目からは同じ出力のほかのサンプルレートのエンドポイントで、失敗してもログに残すだけにする
    // （ほかのエンドポイントは、それぞれ失敗したときに自分の変更を戻すので、中途半端には残らない）
    const bool guidsValid = args.size() >= 2 && std::all_of(args.begin() + 1, args.end(), LooksLikeGuid);

    // インストーラーから呼ぶ命令（Windows Audio の再起動は、それぞれの中で必要なときだけ行う）
    if (args.size() == 1 || (args.size() == 2 && (args[0] == L"register" || args[0] == L"quit-app")))
    {
        int installerResult = -1;
        if (args[0] == L"quit-app")
            installerResult = QuitApp(args.size() == 2 ? args[1] : std::wstring());
        else if (args[0] == L"stop-audio")
            installerResult = StopAudioCommand();
        else if (args[0] == L"start-audio")
            installerResult = StartAudioCommand();
        else if (args[0] == L"register" && args.size() == 2)
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // regsvr32 と同じく COM を使える状態で呼ぶ
            installerResult = Register(args[1]);
        }
        else if (args[0] == L"uninstall")
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            installerResult = Uninstall();
        }

        if (installerResult >= 0)
        {
            CloseLog();
            return installerResult;
        }
    }

    int result = 2;
    int otherFailures = 0;
    if (guidsValid && (args[0] == L"attach" || args[0] == L"detach"))
    {
        const bool attach = args[0] == L"attach";
        result = attach ? Attach(args[1]) : Detach(args[1]);

        if (result == 0)
            for (size_t i = 2; i < args.size(); ++i)
                if ((attach ? Attach(args[i]) : Detach(args[i])) != 0)
                {
                    ++otherFailures;
                    Print(L"warning: %ls failed for the other-rate endpoint %ls (the selected device was switched)",
                          attach ? L"attach" : L"detach", args[i].c_str());
                }
    }
    else if (args.size() == 1 && args[0] == L"detach-all")
        result = DetachAll();
    else
        std::fwprintf(stderr, L"usage: PRISystemRackSetup attach|detach <{endpoint GUID}> [<{GUID}> ...] [--no-restart]\n"
                              L"       PRISystemRackSetup detach-all [--no-restart]\n"
                              L"       PRISystemRackSetup quit-app [<app exe>] | stop-audio | start-audio | register <APO DLL> | uninstall\n");

    if (result == 0 && restart)
        result = RestartAudioService();

    // 選んだデバイスは切り替えて反映も済んだが、ほかのレートのどれかで失敗した
    if (result == 0 && otherFailures > 0)
        result = 3;

    CloseLog();
    return result;
}
