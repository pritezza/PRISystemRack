#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pri::host
{
struct RenderEndpoint
{
    std::wstring friendlyName; // 例: "CABLE Input (VB-Audio Virtual Cable)"
    std::wstring guid;         // PKEY_AudioEndpoint_GUID を大文字にしたもの。APO との共有メモリの名前に使う
    bool apoInstalled = false; // PRISystemRack APO がミックス後の枠に登録されているか
    bool otherApoPresent = false; // ほかの APO（Equalizer APO やドライバーの音響効果など）が入っているか
    std::wstring otherApo;        // その名前（設定画面に出る名前。例: "Equalizer APO"）。分からなければ空

    // PRISystemRack を入れたときに控えた元の登録（HKLM\SOFTWARE\PRISystemRack\EndpointBackup\{GUID}）。外すとこれが戻る
    bool backupPresent = false;
    bool backupHasApo = false; // 控えにほかの APO があるか
    std::wstring backupApo;    // その名前。分からなければ空

    // 同じ機器の同じ出力を表すキー（親のデバイスのインスタンス ID と、表示名（例: "Line (AG06/AG03)"））。分からなければ空。
    // サンプルレートごとに別のエンドポイントを作るドライバー（AG06 の Yamaha Steinberg USB Driver など）では、
    // ASIO のアプリがレートを変えると、GUID の違う「同じ出力」に入れ替わる。それを同じデバイスとして扱うのに使う
    std::wstring deviceKey;
    bool active = true; // 今使える（DEVICE_STATE_ACTIVE）か。includeInactive のときだけ false のものがある

    // APO を入れられるか（HKLM の MMDevices\Audio\Render\{GUID} がある）。リモート デスクトップの「リモート オーディオ」のような、
    // セッションごとの仮想のデバイスはここに載らず、Setup ツールが「見つからない」で失敗する（2026-11-14、開発用とは別の Windows 10 の PC で確認）
    bool canHaveApo = true;
};

// 再生デバイスの一覧（COM が未初期化のスレッドでは、この中で初期化して戻す）。
// 既定は有効なものだけ。includeInactive で、今はない（取り外した、別のレートに入れ替わった）ものや無効にしたものも含める
std::vector<RenderEndpoint> ListRenderEndpoints(bool includeInactive = false);

// guid と同じ機器の同じ出力（deviceKey が同じ）の、ほかのエンドポイント（今ないものも含む）。guid 自身は含めない
std::vector<RenderEndpoint> FindSiblingEndpoints(const std::wstring& guid);

// guid のエンドポイントが今使えないとき、同じ出力で今使えるものが 1 つだけあれば、それを返す（2 つ以上なら決められないので返さない）
std::optional<RenderEndpoint> FindActiveSuccessor(const std::wstring& guid);

// 名前が namePrefix で始まる再生デバイスを探す。0 個または複数なら見つからない扱い
std::optional<RenderEndpoint> FindRenderEndpoint(const std::wstring& namePrefix);
} // namespace pri::host
