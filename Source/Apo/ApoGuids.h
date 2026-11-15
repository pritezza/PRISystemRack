#pragma once

#include <guiddef.h>

namespace pri::apo
{
// PRISystemRack APO（ミックス後の枠に入れる）
// {C06E99BA-ABC5-4FB7-9943-0306C073ADA5}
inline constexpr CLSID kClsidPriSystemRackApo = {
    0xc06e99ba, 0xabc5, 0x4fb7, { 0x99, 0x43, 0x03, 0x06, 0xc0, 0x73, 0xad, 0xa5 } };

inline constexpr wchar_t kApoFriendlyName[] = L"PRISystemRack APO";

// APO が読む設定（HKEY_LOCAL_MACHINE 以下）
inline constexpr wchar_t kApoRegistryKey[] = L"SOFTWARE\\PRISystemRack\\APO";
} // namespace pri::apo
