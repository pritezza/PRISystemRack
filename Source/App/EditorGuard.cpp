#include "EditorGuard.h"

#include "AppLog.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
constexpr UINT_PTR subclassId = 0x50524947; // 'PRIG'

struct State
{
    HWND root = nullptr;             // PluginWindow の窓（プラグインの DLL ごとの状態では nullptr）
    std::function<void()> onFault;
    bool faulted = false;
    std::set<HWND> wrapped;          // 包んでいる子ウィンドウ
};

// PluginWindow（Component）ごとの状態。消すのは release のとき
std::map<const juce::Component*, std::unique_ptr<State>>& states()
{
    static std::map<const juce::Component*, std::unique_ptr<State>> map;
    return map;
}

// プラグインの DLL ごとの状態（PluginWindow の外に、プラグインが自分で作った窓。DodgePro は画面とは別の窓の処理の中で
// 0 で割って落ちた）。消さない
std::map<HMODULE, std::unique_ptr<State>>& moduleStates()
{
    static std::map<HMODULE, std::unique_ptr<State>> map;
    return map;
}

HHOOK cbtHook = nullptr;
bool hookForWholeApp = false; // installForPlugins で入れた（release で外さない）

State* findStateForRoot (HWND root)
{
    for (auto& [component, state] : states())
        if (state->root == root)
            return state.get();
    return nullptr;
}

// 窓の処理（いちばん外側の関数）がある DLL。ANSI の窓は A 版で取らないと、関数のアドレスではなく
// 変換用のハンドルが返る（古いプラグインには、A 版で作った窓や差し替えた処理がある）
HMODULE procModuleOf (HWND hwnd)
{
    const auto proc = IsWindowUnicode (hwnd) ? GetWindowLongPtrW (hwnd, GWLP_WNDPROC) : GetWindowLongPtrA (hwnd, GWLP_WNDPROC);
    HMODULE module = nullptr;
    if (proc == 0
        || ! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR> (proc), &module))
        return nullptr;
    return module;
}

// プラグインの DLL の状態（なければ作る）
State& moduleState (HMODULE module, const wchar_t* path);

// 窓の処理がプラグインの DLL の中にあれば、その DLL の状態（なければ作る）。本体と Windows の窓なら nullptr
State* moduleStateFor (HWND hwnd)
{
    HMODULE module = procModuleOf (hwnd);
    if (module == nullptr)
        module = reinterpret_cast<HMODULE> (GetClassLongPtrW (hwnd, GCLP_HMODULE)); // 窓の種類を登録した DLL

    if (module == nullptr || module == GetModuleHandleW (nullptr))
        return nullptr; // 本体

    wchar_t path[MAX_PATH] = {}, windowsFolder[MAX_PATH] = {};
    if (GetModuleFileNameW (module, path, MAX_PATH) == 0)
        return nullptr;
    const UINT folderLength = GetWindowsDirectoryW (windowsFolder, MAX_PATH);
    if (folderLength > 0 && _wcsnicmp (path, windowsFolder, folderLength) == 0)
        return nullptr; // Windows の DLL（コモンコントロール、IME、OpenGL のドライバーなど）

    return &moduleState (module, path);
}

State& moduleState (HMODULE module, const wchar_t* path)
{
    auto& state = moduleStates()[module];
    if (state == nullptr)
    {
        state = std::make_unique<State>();
        const auto name = juce::File (juce::String (path)).getFileName();
        state->onFault = [name]
        {
            AppLog::write ("plugin windows stopped by an error inside " + name);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Plugin error"),
                                                    TRANS ("An error occurred inside %name%. To keep the application running, the windows of this plugin "
                                                           "were stopped. The plugin keeps processing the sound.").replace ("%name%", name)
                                                        + "\n\n" + TRANS ("Restarting the application is recommended."));
        };
    }
    return *state;
}

//==============================================================================
// 例外を受け止めて元の処理を呼ぶ（__try を使う関数には、後始末が要る C++ のオブジェクトを置けないので分ける）
bool callOriginal (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    __try
    {
        *result = DefSubclassProc (hwnd, message, wParam, lParam);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void notifyFault (State& state)
{
    if (state.faulted)
        return;

    state.faulted = true;

    // 今はプラグインの処理の途中（の跡）なので、知らせるのは後で
    if (state.root == nullptr)
    {
        // プラグインの DLL ごとの状態（消さないので、そのまま使える）
        juce::MessageManager::callAsync ([s = &state] { if (s->onFault != nullptr) s->onFault(); });
        return;
    }

    juce::MessageManager::callAsync ([root = state.root]
    {
        if (auto* s = findStateForRoot (root); s != nullptr && s->onFault != nullptr)
            s->onFault();
    });
}

LRESULT CALLBACK guardedProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
{
    auto* state = reinterpret_cast<State*> (refData);

    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass (hwnd, guardedProc, subclassId);
        state->wrapped.erase (hwnd);
        return DefSubclassProc (hwnd, message, wParam, lParam);
    }

    // 例外が起きたあとは、もうプラグインに渡さない（中の状態が壊れているので）。描画の要求だけは済ませて繰り返さないようにする
    if (state->faulted)
    {
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint;
            BeginPaint (hwnd, &paint);
            EndPaint (hwnd, &paint);
            return 0;
        }
        return DefWindowProcW (hwnd, message, wParam, lParam);
    }

    LRESULT result = 0;
    if (callOriginal (hwnd, message, wParam, lParam, &result))
        return result;

    notifyFault (*state);
    return 0;
}

void wrap (State& state, HWND hwnd)
{
    if (hwnd == state.root || state.wrapped.count (hwnd) != 0)
        return;
    if (GetWindowThreadProcessId (hwnd, nullptr) != GetCurrentThreadId())
        return; // 別のスレッドの窓は包めない

    if (SetWindowSubclass (hwnd, guardedProc, subclassId, reinterpret_cast<DWORD_PTR> (&state)))
        state.wrapped.insert (hwnd);
}

BOOL CALLBACK wrapChild (HWND hwnd, LPARAM param)
{
    wrap (*reinterpret_cast<State*> (param), hwnd);
    return TRUE;
}

// このスレッドで窓が作られるたびに呼ばれる。PluginWindow の下に作られた窓なら、その画面の状態で包む。
// それ以外でも、窓の処理がプラグインの DLL の中にあれば、その DLL の状態で包む（プラグインが自分で作った窓）
LRESULT CALLBACK cbtProc (int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_CREATEWND)
    {
        const auto hwnd = reinterpret_cast<HWND> (wParam);
        const auto* create = reinterpret_cast<CBT_CREATEWNDW*> (lParam);
        State* state = nullptr;
        if (create != nullptr && create->lpcs != nullptr && create->lpcs->hwndParent != nullptr && (create->lpcs->style & WS_CHILD) != 0)
            state = findStateForRoot (GetAncestor (create->lpcs->hwndParent, GA_ROOT));
        if (state == nullptr && hookForWholeApp)
            state = moduleStateFor (hwnd);
        if (state != nullptr)
            wrap (*state, hwnd);
    }
    return CallNextHookEx (cbtHook, code, wParam, lParam);
}

//==============================================================================
// 誰も受け止めなかった例外から戻す（recoverUnhandled）。
// 上の包みは、窓が作られたときに外側に付ける。プラグインがそのあとで窓の処理を自分の関数に差し替えると、その関数が
// 包みの外側に来るので、そこで起きた例外は包みに届かない（Transient+ は数値の入力欄をそうして、その中で落ちた: 2026-11-15）。
// そういう例外は最後にアプリの例外フィルター（CrashLog）に届くので、そこで呼び出し経路をたどり、プラグインの窓の処理を
// Windows（user32）が呼んだところまで戻して、窓の処理の戻り値 0 で続ける。
// 飛ばすのはプラグインの関数だけにする（Windows の DLL や本体の途中を飛ばすと、その後始末（ロックの解放、comctl32 の
// サブクラスの管理など）が行われずに残るため）。プラグインより手前で許すのは、プラグインから呼ばれてロックを持たない
// Windows の処理（C++ の例外を投げる処理、メモリーのコピーなど）だけ
struct ModuleRange
{
    DWORD64 begin = 0, end = 0;
    bool contains (DWORD64 address) const { return address >= begin && address < end; }
};

wchar_t appFolder[MAX_PATH] = {};         // 本体の exe があるフォルダー（末尾に \）
wchar_t windowsFolderPath[MAX_PATH] = {}; // Windows のフォルダー（末尾に \）
HMODULE appModule = nullptr;
HMODULE user32Module = nullptr;
ModuleRange appRange, user32Range, ntdllRange; // ローダーの API を呼ばずに見分けるため（installForPlugins で控える）
std::vector<HMODULE> startupModules; // installForPlugins のときに読み込まれていた DLL（並べ替え済み。あとから変えない）。プラグインではない

// DLL（窓の処理を持つプラグインの DLL）ごとの、戻した回数と、もう戻さない印。例外の処理の途中（どのスレッドからでも）で
// 使うので、固定の大きさで atomic にする（メモリーを確保しない）
struct RecoveryEntry
{
    std::atomic<HMODULE> module { nullptr };
    std::atomic<ULONGLONG> windowStart { 0 };
    std::atomic<int> count { 0 };
    std::atomic<bool> refused { false };
};
RecoveryEntry recoveryEntries[32];
constexpr ULONGLONG recoveryWindowMs = 10000;
constexpr int maxRecoveriesPerWindow = 20; // 10 秒に 20 回を超えたら、戻しても同じ所で落ち続けているので、あきらめる

RecoveryEntry* recoveryEntryFor (HMODULE module)
{
    for (auto& entry : recoveryEntries)
        if (entry.module.load() == module)
            return &entry;
    for (auto& entry : recoveryEntries)
    {
        HMODULE expected = nullptr;
        if (entry.module.compare_exchange_strong (expected, module) || expected == module)
            return &entry;
    }
    return nullptr; // 一杯（それだけ多くの DLL で落ちているなら戻さない）
}

ModuleRange rangeOf (HMODULE module)
{
    if (module == nullptr)
        return {};
    const auto* base = reinterpret_cast<const BYTE*> (module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*> (base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*> (base + dos->e_lfanew);
    return { reinterpret_cast<DWORD64> (base), reinterpret_cast<DWORD64> (base) + nt->OptionalHeader.SizeOfImage };
}

enum class Owner
{
    unknown,       // どの DLL でもない
    app,           // 本体（exe と、そのフォルダーの DLL）
    user32,
    ntdll,
    allowedSystem, // プラグインから呼ばれて、ロックを持たない Windows の処理（KernelBase の RaiseException、vcruntime の throw と memcpy など）
    system,        // それ以外の Windows の DLL
    other,         // 起動したときからある、Windows と本体以外の DLL（常駐ツールが入れるものなど）
    plugin
};

Owner ownerOf (DWORD64 address, HMODULE& module)
{
    module = nullptr;

    // ローダーの API を呼ぶ前に、範囲で分かるものは範囲で見分ける（ntdll の中で落ちたときは、ローダーのロックを持っていることがある）
    if (ntdllRange.contains (address))
        return Owner::ntdll;
    if (user32Range.contains (address))
    {
        module = user32Module;
        return Owner::user32;
    }
    if (appRange.contains (address))
        return Owner::app;

    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR> (address), &module)
        || module == nullptr)
        return Owner::unknown;

    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW (module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return Owner::unknown;
    if (_wcsnicmp (path, windowsFolderPath, wcslen (windowsFolderPath)) == 0)
    {
        const wchar_t* name = wcsrchr (path, L'\\');
        name = name != nullptr ? name + 1 : path;
        for (const auto* allowed : { L"kernelbase.dll", L"vcruntime140.dll", L"vcruntime140_1.dll" })
            if (_wcsicmp (name, allowed) == 0)
                return Owner::allowedSystem;
        return Owner::system;
    }
    if (_wcsnicmp (path, appFolder, wcslen (appFolder)) == 0)
        return Owner::app;
    if (std::binary_search (startupModules.begin(), startupModules.end(), module))
        return Owner::other;
    return Owner::plugin;
}

bool isRecoverableCode (DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case 0xE06D7363: // C++ の例外（受け止められずに出てきたもの）
            return true;
        default: // スタックの溢れ、ヒープの破損、浮動小数点の例外（制御ワードが変わったまま）などは戻さない
            return false;
    }
}

// 1 段上の呼び出し元に戻す（例外を処理するときと同じ巻き戻し。片付け（デストラクター）は呼ばない）
void unwindOneFrame (CONTEXT& context)
{
    DWORD64 imageBase = 0;
    auto* function = RtlLookupFunctionEntry (context.Rip, &imageBase, nullptr);
    if (function == nullptr)
    {
        // 巻き戻しの情報がない関数（スタックを使わない末端の関数）か、関数でない所を実行した: スタックの先頭が戻り先
        context.Rip = *reinterpret_cast<const DWORD64*> (context.Rsp);
        context.Rsp += 8;
        return;
    }

    void* handlerData = nullptr;
    DWORD64 establisherFrame = 0;
    RtlVirtualUnwind (UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
}

// 落ちた所から呼び出し元をたどり、プラグインの窓の処理を user32 が呼んだ所まで戻せたら true（context をその所にし、
// procModule に、user32 から呼ばれたプラグインの関数（窓の処理）がある DLL を入れる）。
// 最初のプラグインの関数より手前は allowedSystem だけ、そこから user32 まではプラグインの関数だけを許す
bool findWindowProcCaller (const EXCEPTION_RECORD& record, CONTEXT& context, HMODULE& procModule)
{
    bool inPlugin = false;
    for (int depth = 0; depth < 64; ++depth)
    {
        HMODULE module = nullptr;
        const auto owner = ownerOf (context.Rip, module);

        if (inPlugin)
        {
            if (owner == Owner::user32)
                return true; // プラグインの窓の処理を呼んだ所
            if (owner != Owner::plugin)
                return false; // 間に Windows の DLL（comctl32 など）や本体がある: その後始末を飛ばすことになる
            procModule = module;
        }
        else if (owner == Owner::plugin)
        {
            inPlugin = true;
            procModule = module;
        }
        else if (owner == Owner::unknown)
        {
            // DLL の外を実行しようとした（壊れた関数ポインターを呼んだ）ときだけ、呼んだ側に戻して続ける
            const bool executedBadAddress = depth == 0 && record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2
                                            && record.ExceptionInformation[0] == 8;
            if (! executedBadAddress)
                return false;
        }
        else if (owner != Owner::allowedSystem)
        {
            return false; // ntdll（ヒープやローダーの途中）、本体、ほかの Windows の DLL、常駐ツールなどの DLL
        }

        unwindOneFrame (context);
        if (context.Rip == 0)
            return false;
    }
    return false;
}

// たどる途中でまた例外が起きても（スタックが壊れているなど）、ここで止める（__try を使う関数には、片付けが要るものを置かない）
bool findWindowProcCallerGuarded (const EXCEPTION_RECORD* record, CONTEXT* context, HMODULE* procModule)
{
    __try
    {
        return findWindowProcCaller (*record, *context, *procModule);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// 例外を起こした DLL の窓に、もうメッセージを渡さない（中の状態が壊れているので。描画の要求だけは済ませて繰り返さないようにする）
LRESULT CALLBACK stoppedProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint;
        BeginPaint (hwnd, &paint);
        EndPaint (hwnd, &paint);
        return 0;
    }
    return DefWindowProcW (hwnd, message, wParam, lParam);
}

struct ModuleWindows
{
    HMODULE module = nullptr;
    std::vector<HWND> found;
};

BOOL CALLBACK collectModuleChild (HWND hwnd, LPARAM param)
{
    auto& windows = *reinterpret_cast<ModuleWindows*> (param);
    if (procModuleOf (hwnd) == windows.module)
        windows.found.push_back (hwnd);
    return TRUE;
}

BOOL CALLBACK collectModuleTopLevel (HWND hwnd, LPARAM param)
{
    collectModuleChild (hwnd, param);
    EnumChildWindows (hwnd, collectModuleChild, param);
    return TRUE;
}

bool containsModuleWindow (HWND root, HMODULE module)
{
    ModuleWindows windows { module, {} };
    EnumChildWindows (root, collectModuleChild, reinterpret_cast<LPARAM> (&windows));
    return ! windows.found.empty();
}

// メッセージスレッドで、戻したあとの後始末をする（module = 窓の処理があるプラグインの DLL、threadId = 落ちたスレッド）:
// - 開いている画面のうち、その DLL の窓を含むものは閉じてもらう
// - その DLL の窓（落ちたスレッドのもの）にはもうメッセージを渡さない。本体の窓（JUCE が作った窓）は、プラグインが処理を
//   差し替えていても止めない（本体の画面が動かなくなるため）
// - どちらも無ければ（何も止められず、また同じ所で落ちる）、その DLL ではもう戻さず、次は落ちるに任せる
void handleRecoveredFault (HMODULE module, DWORD code, DWORD64 address, DWORD threadId)
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW (module, path, MAX_PATH) == 0)
        return; // もう外された
    const auto name = juce::File (juce::String (path)).getFileName();

    HMODULE faultModule = nullptr;
    GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR> (address), &faultModule);
    wchar_t faultPath[MAX_PATH] = {};
    if (faultModule != nullptr)
        GetModuleFileNameW (faultModule, faultPath, MAX_PATH);
    AppLog::write ("recovered from an error in a window procedure of " + name
                   + juce::String::formatted (" (exception 0x%08X at ", (unsigned int) code)
                   + (faultModule != nullptr ? juce::File (juce::String (faultPath)).getFileName()
                                                   + juce::String::formatted ("+0x%llX)", (unsigned long long) (address - reinterpret_cast<DWORD64> (faultModule)))
                                             : juce::String::formatted ("0x%llX)", (unsigned long long) address)));

    std::vector<State*> editors; // 窓を止める前に探す（止めると、窓の処理が本体の stoppedProc になる）
    for (auto& [component, state] : states())
        if (state->root != nullptr && containsModuleWindow (state->root, module))
            editors.push_back (state.get());

    ModuleWindows windows { module, {} };
    EnumThreadWindows (threadId, collectModuleTopLevel, reinterpret_cast<LPARAM> (&windows));
    int stopped = 0;
    for (const auto hwnd : windows.found)
        if (reinterpret_cast<HMODULE> (GetClassLongPtrW (hwnd, GCLP_HMODULE)) != appModule
            && SetWindowLongPtrW (hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR> (stoppedProc)) != 0)
            ++stopped;

    for (auto* state : editors)
        notifyFault (*state); // 2 回目からは何もしない

    if (! editors.empty())
        return;

    // 画面の外の窓で起きたときは、DLL ごとに 1 回だけ知らせる。止める前に続けて起きた分が、先の知らせで画面を
    // 閉じたあとに届いても（もう画面がないので）知らせを重ねない
    static std::set<HMODULE> handledModules;
    const bool firstTime = handledModules.insert (module).second;

    if (stopped > 0)
    {
        if (firstTime)
            notifyFault (moduleState (module, path));
        return;
    }

    if (auto* entry = recoveryEntryFor (module))
        entry->refused = true;
    AppLog::write ("no window of " + name + " could be stopped; the next error there will not be recovered");
    if (firstTime)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Plugin error"),
                                                TRANS ("An error occurred inside %name%. The application kept running, but this plugin may not work correctly.")
                                                        .replace ("%name%", name)
                                                    + "\n\n" + TRANS ("Restarting the application is recommended."));
}
} // namespace

namespace EditorGuard
{
void protect (juce::Component& pluginWindow, std::function<void()> onFault)
{
    auto* peer = pluginWindow.getPeer();
    if (peer == nullptr)
        return;

    auto& state = states()[&pluginWindow];
    if (state != nullptr)
    {
        // 前に同じアドレスにあった画面の状態が残っていた（例外で止めた画面が、後始末の前に消されたなど）。
        // そのまま使うと、止めた印が付いたままで新しい画面にメッセージが渡らないので、包みを外して作り直す
        for (const auto hwnd : std::set<HWND> (state->wrapped))
            if (IsWindow (hwnd))
                RemoveWindowSubclass (hwnd, guardedProc, subclassId);
    }
    state = std::make_unique<State>();
    state->root = (HWND) peer->getNativeHandle();
    state->onFault = [onFault = std::move (onFault), windowName = pluginWindow.getName()]
    {
        AppLog::write ("plugin window closed by an error: " + windowName);
        if (onFault != nullptr)
            onFault();
    };

    EnumChildWindows (state->root, wrapChild, reinterpret_cast<LPARAM> (state.get()));

    if (cbtHook == nullptr)
        cbtHook = SetWindowsHookExW (WH_CBT, cbtProc, nullptr, GetCurrentThreadId());
}

void release (const juce::Component* pluginWindow)
{
    auto it = states().find (pluginWindow);
    if (it == states().end())
        return;

    auto& state = *it->second;
    for (const auto hwnd : std::set<HWND> (state.wrapped))
        if (IsWindow (hwnd))
            RemoveWindowSubclass (hwnd, guardedProc, subclassId);
    states().erase (it);

    if (states().empty() && cbtHook != nullptr && ! hookForWholeApp)
    {
        UnhookWindowsHookEx (cbtHook);
        cbtHook = nullptr;
    }
}

void installForPlugins()
{
    // recoverUnhandled で使う（例外の処理の途中では、なるべく何も用意しないで済むように先に調べておく）
    appModule = GetModuleHandleW (nullptr);
    user32Module = GetModuleHandleW (L"user32.dll");
    appRange = rangeOf (appModule);
    user32Range = rangeOf (user32Module);
    ntdllRange = rangeOf (GetModuleHandleW (L"ntdll.dll"));

    // 今読み込まれている DLL（まだプラグインは読み込んでいない）。Windows と本体以外のもの（常駐ツールが入れる DLL など）は
    // プラグインとみなさない
    {
        std::vector<HMODULE> modules (1024);
        DWORD needed = 0;
        if (K32EnumProcessModules (GetCurrentProcess(), modules.data(), (DWORD) (modules.size() * sizeof (HMODULE)), &needed))
        {
            modules.resize (std::min<size_t> (modules.size(), needed / sizeof (HMODULE)));
            std::sort (modules.begin(), modules.end());
            startupModules = std::move (modules);
        }
    }

    if (const DWORD length = GetModuleFileNameW (nullptr, appFolder, MAX_PATH); length > 0 && length < MAX_PATH)
    {
        if (auto* lastSeparator = wcsrchr (appFolder, L'\\'))
            lastSeparator[1] = 0;
    }
    else
    {
        appFolder[0] = 0;
    }
    if (const UINT length = GetWindowsDirectoryW (windowsFolderPath, MAX_PATH - 1); length > 0 && length < MAX_PATH - 1)
    {
        if (windowsFolderPath[length - 1] != L'\\')
            wcscat_s (windowsFolderPath, L"\\");
    }
    else
    {
        windowsFolderPath[0] = 0;
    }

    hookForWholeApp = true;
    if (cbtHook == nullptr)
        cbtHook = SetWindowsHookExW (WH_CBT, cbtProc, nullptr, GetCurrentThreadId());
}

bool hasFaulted (const juce::Component& pluginWindow)
{
    const auto it = states().find (&pluginWindow);
    return it != states().end() && it->second->faulted;
}

namespace
{
void invoke (const std::function<void()>& fn)
{
    fn();
}

bool invokeGuarded (const std::function<void()>& fn)
{
    __try
    {
        invoke (fn);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
} // namespace

bool callGuarded (const std::function<void()>& fn)
{
    return invokeGuarded (fn);
}

bool recoverUnhandled (EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr
        || user32Module == nullptr || appFolder[0] == 0 || windowsFolderPath[0] == 0)
        return false;

    const auto& record = *info->ExceptionRecord;
    if ((record.ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0 || ! isRecoverableCode (record.ExceptionCode))
        return false;

    CONTEXT context = *info->ContextRecord;
    HMODULE procModule = nullptr;
    if (! findWindowProcCallerGuarded (&record, &context, &procModule) || procModule == nullptr)
        return false;

    // 同じ DLL で、もう戻さないと決めたもの・短い時間に戻しすぎたものは、落ちるに任せる（クラッシュの記録が残る）
    auto* entry = recoveryEntryFor (procModule);
    if (entry == nullptr || entry->refused.load())
        return false;
    const auto now = GetTickCount64();
    if (now - entry->windowStart.load() > recoveryWindowMs)
    {
        entry->windowStart = now;
        entry->count = 0;
    }
    if (++entry->count > maxRecoveriesPerWindow)
    {
        entry->refused = true;
        return false;
    }

    // 窓の処理が 0 を返したことにして、user32 の続きから動かす（揮発性のレジスターは、呼び出しから戻ったあとは使われない）
    context.Rax = 0;
    *info->ContextRecord = context;

    // 知らせと後始末はメッセージスレッドで（今は例外の処理の途中）
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        juce::MessageManager::callAsync ([procModule, code = record.ExceptionCode, address = reinterpret_cast<DWORD64> (record.ExceptionAddress),
                                          threadId = GetCurrentThreadId()]
                                         { handleRecoveredFault (procModule, code, address, threadId); });
    return true;
}
} // namespace EditorGuard
