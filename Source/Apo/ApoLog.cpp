#include "ApoLog.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace pri::apo
{
namespace
{
constexpr LONGLONG kMaxLogBytes = 1024 * 1024; // apo.log がこれを超えたら apo.old.log に回す

bool GetLogPath(wchar_t* path, size_t count)
{
    wchar_t programData[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    return swprintf_s(path, count, L"%ls\\PRISystemRack\\Logs\\apo.log", programData) > 0;
}
} // namespace

void Log(const wchar_t* format, ...)
{
    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);

    wchar_t line[1280];
    swprintf_s(line, _countof(line), L"%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu tid %lu] %ls\r\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
               GetCurrentProcessId(), GetCurrentThreadId(), message);

    OutputDebugStringW(line);

    char utf8[4096];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (bytes <= 1)
        return;

    wchar_t path[MAX_PATH];
    if (!GetLogPath(path, MAX_PATH))
        return;

    // 同じプロセスの複数のスレッドが同時にローテーションしないよう、開いてから書き終えるまでを 1 つずつにする
    // （ログは音の処理の周期ごとには書かないので、待っても音は止まらない）
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);

    // ログフォルダはインストール時に作り、LOCAL SERVICE（audiodg.exe の実行アカウント）に書き込みを許可しておく。
    // 名前を変える（ローテーション）ために DELETE の権限も付けて開く（付けられなければ回さずに追記だけする）
    const auto open = [&path](DWORD access)
    {
        return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    bool canRename = true;
    HANDLE file = open(FILE_APPEND_DATA | DELETE);
    if (file == INVALID_HANDLE_VALUE)
    {
        canRename = false;
        file = open(FILE_APPEND_DATA);
    }

    if (file != INVALID_HANDLE_VALUE && canRename)
    {
        // 上限を超えていたら apo.old.log に回して（前の apo.old.log は消える）新しく書き始める。ファイルは最大 2 つ。
        // パスではなく開いたファイルそのものの名前を変える: ほかのプロセス（audiodg.exe と、登録中の Setup など）が先に回して
        // 新しい apo.log を作っていても、それを apo.old.log に上書きしない（このファイルはもう apo.old.log なので、同じ名前への変更になるだけ）
        LARGE_INTEGER size = {};
        wchar_t oldPath[MAX_PATH];
        wchar_t* extension = nullptr;
        if (GetFileSizeEx(file, &size) && size.QuadPart >= kMaxLogBytes && swprintf_s(oldPath, MAX_PATH, L"%ls", path) > 0
            && (extension = wcsrchr(oldPath, L'.')) != nullptr && swprintf_s(extension, MAX_PATH - (extension - oldPath), L".old.log") > 0)
        {
            const size_t nameBytes = wcslen(oldPath) * sizeof(wchar_t);
            alignas(FILE_RENAME_INFO) BYTE buffer[sizeof(FILE_RENAME_INFO) + MAX_PATH * sizeof(wchar_t)] = {};
            auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer);
            rename->ReplaceIfExists = TRUE;
            rename->RootDirectory = nullptr;
            rename->FileNameLength = static_cast<DWORD>(nameBytes);
            memcpy(rename->FileName, oldPath, nameBytes);
            SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(sizeof(FILE_RENAME_INFO) + nameBytes));
            // 失敗しても（ほかのプロセスが使用中など）そのまま追記する
            CloseHandle(file);
            file = open(FILE_APPEND_DATA);
        }
    }

    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
        CloseHandle(file);
    }

    ReleaseSRWLockExclusive(&lock);
}
} // namespace pri::apo
