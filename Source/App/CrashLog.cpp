#include "CrashLog.h"

#include "EditorGuard.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace
{
wchar_t crashFolder[MAX_PATH] = {};

// 落ちた時刻の名前（crash-20261105-224512）
void makeBaseName (wchar_t* out, size_t count)
{
    SYSTEMTIME t {};
    GetLocalTime (&t);
    swprintf_s (out, count, L"%s\\crash-%04u%02u%02u-%02u%02u%02u", crashFolder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

void writeMinidump (const wchar_t* baseName, EXCEPTION_POINTERS* info)
{
    wchar_t path[MAX_PATH];
    swprintf_s (path, L"%s.dmp", baseName);
    const HANDLE file = CreateFileW (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION exception {};
    exception.ThreadId = GetCurrentThreadId();
    exception.ExceptionPointers = info;
    exception.ClientPointers = FALSE;
    MiniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(), file,
                       (MINIDUMP_TYPE) (MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                       info != nullptr ? &exception : nullptr, nullptr, nullptr);
    CloseHandle (file);
}

// 落ちた場所からの呼び出し経路を、関数名・ファイル・行つきで書く
void writeStack (const wchar_t* baseName, EXCEPTION_POINTERS* info)
{
    wchar_t path[MAX_PATH];
    swprintf_s (path, L"%s.txt", baseName);
    FILE* out = nullptr;
    if (_wfopen_s (&out, path, L"w") != 0 || out == nullptr)
        return;

    const auto* record = info->ExceptionRecord;
    std::fprintf (out, "exception 0x%08lX at %p (thread %lu)\n", record->ExceptionCode, record->ExceptionAddress, GetCurrentThreadId());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        std::fprintf (out, "access violation: %s address %p\n", record->ExceptionInformation[0] == 0 ? "read" : "write",
                      (void*) record->ExceptionInformation[1]);
    std::fprintf (out, "\n");

    const HANDLE process = GetCurrentProcess();
    SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize (process, nullptr, TRUE);

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas (SYMBOL_INFO) char symbolBuffer[sizeof (SYMBOL_INFO) + 512] {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*> (symbolBuffer);

    for (int depth = 0; depth < 64; ++depth)
    {
        if (! StackWalk64 (IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr)
            || frame.AddrPC.Offset == 0)
            break;

        const DWORD64 address = frame.AddrPC.Offset;
        char module[MAX_PATH] = "?";
        if (const DWORD64 base = SymGetModuleBase64 (process, address))
        {
            char modulePath[MAX_PATH] = {};
            if (GetModuleFileNameA ((HMODULE) base, modulePath, MAX_PATH) > 0)
            {
                const char* name = std::strrchr (modulePath, '\\');
                strcpy_s (module, name != nullptr ? name + 1 : modulePath);
            }
        }

        symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const bool hasSymbol = SymFromAddr (process, address, &displacement, symbol) != FALSE;

        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof (line);
        DWORD lineDisplacement = 0;
        const bool hasLine = SymGetLineFromAddr64 (process, address, &lineDisplacement, &line) != FALSE;

        std::fprintf (out, "%2d  %s!%s+0x%llx", depth, module, hasSymbol ? symbol->Name : "?", (unsigned long long) displacement);
        if (hasLine)
            std::fprintf (out, "  (%s:%lu)", line.FileName, line.LineNumber);
        std::fprintf (out, "\n");
    }

    SymCleanup (process);
    std::fclose (out);
}

volatile LONG entered = 0; // 一度書いたら、もう書かない（書いている最中にまた落ちたときも）

LONG WINAPI onUnhandledException (EXCEPTION_POINTERS* info)
{
    // プラグインの窓の処理の中で起きたものなら、そこから戻してアプリを動かし続ける（記録はアプリのログに書く）
    if (EditorGuard::recoverUnhandled (info))
        return EXCEPTION_CONTINUE_EXECUTION;

    if (InterlockedExchange (&entered, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t baseName[MAX_PATH];
    makeBaseName (baseName, MAX_PATH);
    writeStack (baseName, info);
    writeMinidump (baseName, info);
    return EXCEPTION_CONTINUE_SEARCH; // あとは Windows に任せる（エラー報告に記録される）
}

// ヒープの破損（0xC0000374）は、起きた瞬間（どの __except よりも先）に記録する。
// プラグインの呼び出しや画面は __except (EXCEPTION_EXECUTE_HANDLER) で守っているので、そこで受け止められると
// 上の onUnhandledException にも Windows のエラー報告にも届かず、何も残らずに終わってしまう（UFX-REVERB で 2026-11-06 に確認）。
// ヒープが壊れているので書けないこともある。先にダンプ、次に呼び出し経路を書く
LONG WINAPI onFirstChanceException (EXCEPTION_POINTERS* info)
{
    constexpr DWORD heapCorruption = 0xC0000374;
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ExceptionRecord->ExceptionCode != heapCorruption)
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedExchange (&entered, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t baseName[MAX_PATH];
    makeBaseName (baseName, MAX_PATH);
    writeMinidump (baseName, info);
    writeStack (baseName, info);
    return EXCEPTION_CONTINUE_SEARCH; // 扱いは変えない
}
} // namespace

namespace CrashLog
{
juce::File folder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("PRISystemRack").getChildFile ("Crashes");
}

void install()
{
    const auto dir = folder();
    dir.createDirectory();

    // 古い記録を消して新しい keepCount 件だけ残す（ダンプは 1 件 0.5〜0.8 MB あり、放っておくと溜まり続けるため）。
    // 名前は crash-<年月日>-<時分秒> なので、名前の順が時刻の順になる。.txt と .dmp は同じ名前で 1 件と数える
    constexpr int keepCount = 10;
    juce::StringArray names;
    for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, "crash-*"))
        names.addIfNotAlreadyThere (file.getFileNameWithoutExtension());
    names.sort (false);
    for (int i = 0; i < names.size() - keepCount; ++i)
        for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, names[i] + ".*"))
            file.deleteFile();
    wcsncpy_s (crashFolder, dir.getFullPathName().toWideCharPointer(), _TRUNCATE);
    SetUnhandledExceptionFilter (onUnhandledException);
    AddVectoredExceptionHandler (1, onFirstChanceException);
}

void reinstall()
{
    if (crashFolder[0] != 0)
        SetUnhandledExceptionFilter (onUnhandledException);
}
} // namespace CrashLog
