#include "PluginBinaryCheck.h"

#ifndef NOMINMAX
 #define NOMINMAX // std::max を使う
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <vector>

#pragma comment(lib, "version.lib") // GetFileVersionInfoW（Visual C++ ランタイムの版）

namespace pri::host
{
namespace
{
bool IsFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsDirectory(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring FolderOf(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Lower(std::wstring text)
{
    for (auto& c : text)
        c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

// フォルダー形式の中の、ある CPU 用の DLL（Contents\<arch>\<名前>.vst3。名前が違えば最初の .vst3）
std::wstring FindBundleBinary(const std::wstring& bundle, const wchar_t* archFolder)
{
    const std::wstring folder = bundle + L"\\Contents\\" + archFolder;
    std::wstring stem = bundle.substr(bundle.find_last_of(L"\\/") + 1);
    if (const auto dot = stem.find_last_of(L'.'); dot != std::wstring::npos)
        stem = stem.substr(0, dot);

    if (const std::wstring expected = folder + L"\\" + stem + L".vst3"; IsFile(expected))
        return expected;

    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileW((folder + L"\\*.vst3").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return {};
    FindClose(search);
    return folder + L"\\" + found.cFileName;
}

// ファイルの必要な所だけを ReadFile で読む（実行はしない）。メモリーに写して読むと、ネットワークのドライブが途中で切れたときに
// 読み取りの例外（EXCEPTION_IN_PAGE_ERROR）で本体ごと落ちるので、写さない（ReadFile なら失敗が返るだけ）
class FileReader
{
public:
    explicit FileReader(const std::wstring& path)
    {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size = {};
        if (file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size) && size.QuadPart > 0)
            size_ = static_cast<uint64_t>(size.QuadPart);
    }

    ~FileReader()
    {
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
    }

    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;

    uint64_t Size() const { return size_; }

    // offset から count バイトを読む（ファイルの中に収まっていて、全部読めたときだけ true）
    bool Read(uint64_t offset, void* out, DWORD count) const
    {
        if (size_ == 0 || offset > size_ || count > size_ - offset)
            return false;
        LARGE_INTEGER position = {};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD read = 0;
        return SetFilePointerEx(file_, position, nullptr, FILE_BEGIN) && ReadFile(file_, out, count, &read, nullptr) && read == count;
    }

    template <typename T>
    bool Read(uint64_t offset, T& out) const
    {
        return Read(offset, &out, sizeof(T));
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

struct PeInfo
{
    bool valid = false;
    WORD machine = 0;
    int linkerMajor = 0, linkerMinor = 0;
    std::vector<std::wstring> imports; // 小文字
};

PeInfo ReadPe(const std::wstring& path)
{
    PeInfo info;
    const FileReader file(path);

    IMAGE_DOS_HEADER dos = {};
    if (!file.Read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
        return info;

    const uint64_t ntOffset = static_cast<uint64_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header = {};
    if (!file.Read(ntOffset, signature) || signature != IMAGE_NT_SIGNATURE || !file.Read(ntOffset + sizeof(DWORD), header))
        return info;

    info.valid = true;
    info.machine = header.Machine;

    const uint64_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    IMAGE_OPTIONAL_HEADER64 optional = {};
    if (!file.Read(optionalOffset, optional) || optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return info; // 32 ビットなどは machine だけで判断する

    info.linkerMajor = optional.MajorLinkerVersion;
    info.linkerMinor = optional.MinorLinkerVersion;

    // セクションの表（RVA をファイルの位置に直すのに使う）
    std::vector<IMAGE_SECTION_HEADER> sections(header.NumberOfSections);
    if (sections.empty()
        || !file.Read(optionalOffset + header.SizeOfOptionalHeader, sections.data(),
                      static_cast<DWORD>(sizeof(IMAGE_SECTION_HEADER) * sections.size())))
        return info;

    const auto toOffset = [&](DWORD rva) -> uint64_t
    {
        for (const auto& section : sections)
        {
            const DWORD extent = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress && rva - section.VirtualAddress < extent)
                return static_cast<uint64_t>(section.PointerToRawData) + (rva - section.VirtualAddress);
        }
        return UINT64_MAX;
    };

    if (optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return info;
    const auto& directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0)
        return info;

    uint64_t offset = toOffset(directory.VirtualAddress);
    for (int count = 0; count < 4096 && offset != UINT64_MAX; ++count, offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        IMAGE_IMPORT_DESCRIPTOR descriptor = {};
        if (!file.Read(offset, descriptor) || descriptor.Name == 0)
            break;

        const uint64_t nameOffset = toOffset(descriptor.Name);
        if (nameOffset >= file.Size())
            continue;
        char name[MAX_PATH + 1] = {};
        const DWORD length = static_cast<DWORD>(std::min<uint64_t>(file.Size() - nameOffset, MAX_PATH));
        if (!file.Read(nameOffset, name, length))
            continue;

        std::wstring dll;
        for (size_t i = 0; i < length && name[i] != 0; ++i)
            dll += static_cast<wchar_t>(std::tolower(static_cast<unsigned char>(name[i])));
        if (!dll.empty())
            info.imports.push_back(dll);
    }
    return info;
}

bool StartsWith(const std::wstring& text, const wchar_t* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// Visual C++ ランタイムの DLL（再頒布可能パッケージで入るもの）
bool IsVcRuntime(const std::wstring& dll)
{
    return StartsWith(dll, L"msvcp140") || StartsWith(dll, L"vcruntime140") || StartsWith(dll, L"concrt140")
           || StartsWith(dll, L"vcomp140") || StartsWith(dll, L"vccorlib140");
}

std::wstring SystemFolder()
{
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(path) : std::wstring();
}

// ファイルの版（例: 14.28.29910）。読めなければ major は 0
struct FileVersion
{
    int major = 0, minor = 0, build = 0;
};

FileVersion ReadFileVersion(const std::wstring& path)
{
    FileVersion version;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return version;
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())
        && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) && fixed != nullptr)
    {
        version.major = HIWORD(fixed->dwFileVersionMS);
        version.minor = LOWORD(fixed->dwFileVersionMS);
        version.build = HIWORD(fixed->dwFileVersionLS);
    }
    return version;
}

// Windows が DLL を探す場所のどこかにあるか。JUCE（と補助プログラム）は VST3 を素の LoadLibrary で読み込むので、
// 依存する DLL はプラグインのフォルダーからは探されず、本体のフォルダー、System32、Windows、PATH の順に探される
bool CanFindDll(const std::wstring& dll)
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    wchar_t windowsFolder[MAX_PATH] = {};
    GetWindowsDirectoryW(windowsFolder, MAX_PATH);

    for (const auto& folder : { FolderOf(exePath), SystemFolder(), std::wstring(windowsFolder) })
        if (!folder.empty() && IsFile(folder + L"\\" + dll))
            return true;

    wchar_t found[MAX_PATH] = {};
    return SearchPathW(nullptr, dll.c_str(), nullptr, MAX_PATH, found, nullptr) > 0;
}

std::wstring MachineName(WORD machine)
{
    switch (machine)
    {
        case IMAGE_FILE_MACHINE_I386:  return L"x86 (32-bit)";
        case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
        case IMAGE_FILE_MACHINE_ARMNT: return L"ARM";
        default:
        {
            wchar_t text[16];
            swprintf_s(text, L"0x%04X", machine);
            return text;
        }
    }
}
} // namespace

std::wstring ResolvePluginBinary(const std::wstring& vst3Path)
{
    if (IsDirectory(vst3Path))
        return FindBundleBinary(vst3Path, L"x86_64-win");
    return vst3Path;
}

PluginBinaryProblem CheckPluginBinary(const std::wstring& vst3Path)
{
    PluginBinaryProblem problem;

    // フォルダー形式で x64 用がなければ、ほかの CPU 用だけ入っている
    const std::wstring binary = ResolvePluginBinary(vst3Path);
    if (binary.empty())
    {
        if (IsDirectory(vst3Path) && !FindBundleBinary(vst3Path, L"x86-win").empty())
        {
            problem.kind = PluginBinaryProblem::Kind::notX64;
            problem.detail = MachineName(IMAGE_FILE_MACHINE_I386);
        }
        return problem;
    }

    const auto pe = ReadPe(binary);
    if (!pe.valid)
        return problem; // 読めないものは補助プログラムに任せる

    if (pe.machine != IMAGE_FILE_MACHINE_AMD64)
    {
        problem.kind = PluginBinaryProblem::Kind::notX64;
        problem.detail = MachineName(pe.machine);
        return problem;
    }

    // Visual C++ ランタイム（再頒布可能パッケージで System32 に入るもの）。プラグインのフォルダーに同じ DLL があっても使われない（CanFindDll）
    for (const auto& dll : pe.imports)
    {
        if (!IsVcRuntime(dll))
            continue;

        // 入っていない
        if (!CanFindDll(dll))
        {
            problem.kind = PluginBinaryProblem::Kind::runtimeMissing;
            problem.detail = dll;
            return problem;
        }

        // C++ の標準ライブラリ（MSVCP140*.dll）: ビルドしたツールの版（リンカーの版。例: 14.51）より、この PC の版が古いと読み込めない・落ちる
        // （Microsoft の決まりで、ランタイムはビルドに使ったツールと同じか新しい版が要る。2026-11-14 に試した、開発用とは別の Windows 10 の PC は 14.28 で、14.51 のプラグインが
        // 調べている途中で落ちた）。VCRUNTIME140.dll だけを使うものは、少し古くても動くことが多いので、ここでは失敗にしない
        if (!StartsWith(dll, L"msvcp140"))
            continue;
        const std::wstring installed = SystemFolder() + L"\\" + dll;
        const auto version = IsFile(installed) ? ReadFileVersion(installed) : FileVersion {};
        if (pe.linkerMajor == 14 && version.major == 14 && version.minor < pe.linkerMinor)
        {
            wchar_t need[32], have[32];
            swprintf_s(need, L"%d.%d", pe.linkerMajor, pe.linkerMinor);
            swprintf_s(have, L"%d.%d.%d", version.major, version.minor, version.build);
            problem.kind = PluginBinaryProblem::Kind::runtimeTooOld;
            problem.detail = need;
            problem.detail2 = have;
            return problem;
        }
    }

    // そのほかの DLL は、ここでは失敗にしない（WinSxS から読み込まれるものなど、ファイルの場所だけでは確かに言えないため）。
    // 補助プログラムが実際に読み込んで失敗したときに、FindUnresolvedImports で見つからない可能性のあるものを添える
    return problem;
}

std::wstring FindUnresolvedImports(const std::wstring& vst3Path)
{
    const std::wstring binary = ResolvePluginBinary(vst3Path);
    if (binary.empty())
        return {};

    std::wstring missing;
    for (const auto& dll : ReadPe(binary).imports)
    {
        // API セット（実体のファイルがない）と、WinSxS から読み込まれる古いランタイム（Visual Studio 2005 / 2008 など）は除く
        if (StartsWith(dll, L"api-ms-") || StartsWith(dll, L"ext-ms-") || StartsWith(dll, L"msvcr8") || StartsWith(dll, L"msvcp8")
            || StartsWith(dll, L"msvcr9") || StartsWith(dll, L"msvcp9") || StartsWith(dll, L"mfc") || StartsWith(dll, L"atl")
            || StartsWith(dll, L"comctl32") || CanFindDll(dll))
            continue;
        missing += (missing.empty() ? L"" : L", ") + dll;
    }
    return missing;
}

std::wstring DescribeWindowsError(uint32_t code)
{
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                  code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0) // 例外のコード（NTSTATUS）は ntdll.dll の文言
        length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                                GetModuleHandleW(L"ntdll.dll"), code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length > 0 && buffer != nullptr ? std::wstring(buffer, length) : std::wstring();
    if (buffer != nullptr)
        LocalFree(buffer);

    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    if (text.find(L'%') != std::wstring::npos)
        return {}; // 値を差し込む前提の文言（例外の場所など）は、そのままでは読めないので使わない
    return text;
}
} // namespace pri::host
