#pragma once

// 再生デバイスの FxProperties（HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{GUID}\FxProperties）の値の名前。
// 本体アプリ（ほかの APO があるかの判定）と Setup ツール（控えて外す・書き戻す）で同じ一覧を使う
namespace pri::fx
{
// ミックス後の枠（PKEY_FX_PostMixEffectClsid）。PRISystemRack APO はここに入る
inline constexpr wchar_t kPostMixValue[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2";

// 設定画面（サウンドの「拡張」タブ）に出る名前。Equalizer APO は "Equalizer APO" を入れる
inline constexpr wchar_t kNameValue[] = L"{b725f130-47ef-101a-a5f1-02608c9eebac},10";

// APO の CLSID が入る枠: LFX(1), GFX(2), UI(3), SFX(5), MFX(6), EFX(7), Composite SFX/MFX/EFX(13〜15)。
// Composite の枠は REG_MULTI_SZ（CLSID の並び）、ほかは REG_SZ
inline constexpr const wchar_t* kApoSlotValues[] = {
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1",  L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3",  L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6",  L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15",
};

// 控えのキー（HKLM\SOFTWARE\PRISystemRack\EndpointBackup\{GUID}）。Setup ツールが作り、本体アプリは読むだけ
inline constexpr wchar_t kBackupRoot[] = L"SOFTWARE\\PRISystemRack\\EndpointBackup";
} // namespace pri::fx
