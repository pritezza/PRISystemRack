#include "ProcessTools.h"

#include <windows.h>

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#pragma comment(lib, "advapi32.lib") // サービスの状態（GetAudioServiceState）

#include <algorithm>
#include <cwctype>
#include <map>

using Microsoft::WRL::ComPtr;

namespace pri::host
{
namespace
{
struct ProcessEntry
{
    DWORD parent = 0;
    std::wstring name;
};

std::map<DWORD, ProcessEntry> SnapshotProcesses()
{
    std::map<DWORD, ProcessEntry> processes;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return processes;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        processes[entry.th32ProcessID] = { entry.th32ParentProcessID, entry.szExeFile };

    CloseHandle(snapshot);
    return processes;
}

bool SameName(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring Upper(std::wstring text)
{
    for (wchar_t& c : text)
        c = static_cast<wchar_t>(towupper(c));
    return text;
}

constexpr PROPERTYKEY kPkeyAudioEndpointGuid = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4 };
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

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

std::vector<uint32_t> FindRootProcesses(const std::wstring& exeName)
{
    const auto processes = SnapshotProcesses();
    std::vector<uint32_t> roots;

    for (const auto& [pid, entry] : processes)
    {
        if (!SameName(entry.name, exeName))
            continue;

        const auto parent = processes.find(entry.parent);
        const bool parentIsSameApp = parent != processes.end() && parent->first != pid && SameName(parent->second.name, exeName);
        if (!parentIsSameApp)
            roots.push_back(pid);
    }
    return roots;
}

std::set<uint32_t> CollectProcessTree(const std::vector<uint32_t>& roots)
{
    const auto processes = SnapshotProcesses();
    std::multimap<DWORD, DWORD> children;
    for (const auto& [pid, entry] : processes)
        if (entry.parent != pid)
            children.emplace(entry.parent, pid);

    std::set<uint32_t> tree;
    std::vector<DWORD> pending(roots.begin(), roots.end());
    while (!pending.empty())
    {
        const DWORD pid = pending.back();
        pending.pop_back();
        if (!tree.insert(pid).second)
            continue;
        const auto range = children.equal_range(pid);
        for (auto it = range.first; it != range.second; ++it)
            pending.push_back(it->second);
    }
    return tree;
}

bool IsProcessAlive(uint32_t processId)
{
    if (processId == 0)
        return false;
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr)
        return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

AudioServiceState GetAudioServiceState()
{
    AudioServiceState state;
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
        return state;

    if (const SC_HANDLE service = OpenServiceW(manager, L"Audiosrv", SERVICE_QUERY_STATUS))
    {
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed))
        {
            state.known = true;
            state.servicePid = status.dwCurrentState == SERVICE_RUNNING ? status.dwProcessId : 0;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);

    if (state.known)
        for (const auto& [pid, entry] : SnapshotProcesses())
            if (SameName(entry.name, L"audiodg.exe"))
                state.audiodgPid = pid;
    return state;
}

std::vector<AudioSessionInfo> ListAudioSessions()
{
    std::vector<AudioSessionInfo> sessions;

    // このスレッドで COM がまだなら初期化する
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
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))
        || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return sessions;

    const DWORD self = GetCurrentProcessId();
    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);

    // 動いているセッションの音量メーター（あとでまとめて読む。sessions の番号と組にする）
    std::vector<std::pair<size_t, ComPtr<IAudioMeterInformation>>> meters;

    for (UINT d = 0; d < deviceCount; ++d)
    {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> store;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> enumeratorOfSessions;
        if (FAILED(devices->Item(d, &device)) || FAILED(device->OpenPropertyStore(STGM_READ, &store))
            || FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(manager.GetAddressOf())))
            || FAILED(manager->GetSessionEnumerator(&enumeratorOfSessions)))
            continue;

        const std::wstring guid = Upper(ReadString(store.Get(), kPkeyAudioEndpointGuid));
        const std::wstring name = ReadString(store.Get(), kPkeyDeviceFriendlyName);

        int count = 0;
        enumeratorOfSessions->GetCount(&count);
        for (int s = 0; s < count; ++s)
        {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            DWORD pid = 0;
            if (FAILED(enumeratorOfSessions->GetSession(s, &control)) || FAILED(control.As(&control2))
                || control2->IsSystemSoundsSession() == S_OK || FAILED(control2->GetProcessId(&pid)) || pid == 0
                || pid == self)
                continue;

            AudioSessionState state = AudioSessionStateInactive;
            control->GetState(&state);
            AudioSessionInfo info;
            info.processId = pid;
            info.endpointGuid = guid;
            info.endpointName = name;
            info.active = state == AudioSessionStateActive;
            sessions.push_back(info);

            ComPtr<IAudioMeterInformation> meter;
            if (info.active && SUCCEEDED(control.As(&meter)))
                meters.emplace_back(sessions.size() - 1, meter);
        }
    }

    // 動いているセッションの音量を、15 ms おきに 3 回読んで最大をとる（メーターは直近の短い間の最大を返すので、
    // 1 回だけだと音の切れ目に当たることがある）
    constexpr float soundingThreshold = 3.0e-4f; // -70 dB
    for (int round = 0; round < 3 && !meters.empty(); ++round)
    {
        if (round > 0)
            Sleep(15);
        for (auto& [index, meter] : meters)
        {
            float peak = 0.0f;
            if (SUCCEEDED(meter->GetPeakValue(&peak)) && peak > sessions[index].peak)
                sessions[index].peak = peak;
        }
    }
    for (auto& session : sessions)
        session.sounding = session.active && session.peak > soundingThreshold;

    return sessions;
}

std::vector<ProcessGroup> ListProcessGroups()
{
    const auto processes = SnapshotProcesses();
    const auto sessions = ListAudioSessions();
    const DWORD self = GetCurrentProcessId();

    struct Less
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, ProcessGroup, Less> groups;

    for (const auto& [pid, entry] : processes)
    {
        if (pid == 0 || pid == 4 || pid == self || entry.name.empty())
            continue;
        auto& group = groups[entry.name];
        group.exeName = entry.name;
        ++group.processCount;
    }

    for (const auto& session : sessions)
    {
        const auto process = processes.find(session.processId);
        if (process == processes.end())
            continue;
        auto& group = groups[process->second.name];
        group.exeName = process->second.name;
        group.hasAudioSession = true;
        if (session.sounding // 無音で開いているだけのセッションは「再生中」に出さない
            && std::find(group.activeEndpoints.begin(), group.activeEndpoints.end(), session.endpointName) == group.activeEndpoints.end())
            group.activeEndpoints.push_back(session.endpointName);
    }

    std::vector<ProcessGroup> result;
    for (auto& [name, group] : groups)
        result.push_back(std::move(group));
    return result;
}
} // namespace pri::host
