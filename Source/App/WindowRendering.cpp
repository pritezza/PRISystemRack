#include "WindowRendering.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace
{
constexpr int softwareEngine = 0; // JUCE の Windows の描画方式の番号（0 = ソフトウェア、1 = Direct2D）
constexpr int direct2DEngine = 1;
constexpr UINT_PTR subclassId = 0x50524952; // 'PRIR'
constexpr UINT restoreMessage = WM_APP + 0x152; // ドラッグが終わった後で Direct2D に戻す（自分あてに投げる）

// ディスプレイの構成が変わった（抜き差し・解像度・DPI）後も、中身がタイトルバーの高さ分ずれたままになることがある
// （2026-11-08 にディスプレイの電源ケーブルの抜き差しで確認。メニューバーが隠れ、押した位置と描かれた位置がずれる）。
// 構成が変わってからしばらくの間は、ウィンドウが動いたり大きさが変わったりするたびに（PersistentWindows などが後から動かす）、
// 動きが止まってから nudge で描き直させる
constexpr UINT_PTR nudgeTimerId = 0x5052; // 動きが止まるのを待つタイマー
constexpr UINT nudgeDelayMs = 500;
constexpr DWORD watchAfterDisplayChangeMs = 15000;

struct WindowState
{
    DWORD watchUntilMs = 0; // この時刻まで、ウィンドウの動きを見て nudge する
    bool nudging = false;   // nudge で自分が大きさを変えている最中（その WM_WINDOWPOSCHANGED は無視する）
};

juce::ComponentPeer* findPeer (HWND hwnd)
{
    for (int i = juce::ComponentPeer::getNumPeers(); --i >= 0;)
        if (auto* peer = juce::ComponentPeer::getPeer (i); peer != nullptr && peer->getNativeHandle() == hwnd)
            return peer;
    return nullptr;
}

void setEngine (HWND hwnd, int engine)
{
    if (auto* peer = findPeer (hwnd))
        if (peer->getCurrentRenderingEngine() != engine)
            peer->setCurrentRenderingEngine (engine);
}

// プログラムからサイズを変えると中身が正しい位置に描き直されるので、1 ピクセル伸ばして元に戻す
// （最小の大きさのときでも変えられるよう伸ばす方）。最大化しているときは大きさを変えられないので何もしない
void nudge (HWND hwnd, WindowState& state)
{
    RECT r {};
    if (IsZoomed (hwnd) || IsIconic (hwnd) || ! GetWindowRect (hwnd, &r))
        return;

    constexpr UINT flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    const juce::ScopedValueSetter<bool> guard (state.nudging, true);
    SetWindowPos (hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top + 1, flags);
    SetWindowPos (hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, flags);
}

void watchDisplayChange (HWND hwnd, WindowState& state)
{
    state.watchUntilMs = GetTickCount() + watchAfterDisplayChangeMs;
    SetTimer (hwnd, nudgeTimerId, nudgeDelayMs, nullptr);
}

LRESULT CALLBACK windowProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData)
{
    auto& state = *reinterpret_cast<WindowState*> (refData);

    switch (message)
    {
        case WM_DISPLAYCHANGE: // ディスプレイの抜き差し・解像度の変更
        case WM_DPICHANGED:    // 表示の拡大率の違うディスプレイへ移った・拡大率が変わった
            watchDisplayChange (hwnd, state);
            break;

        case WM_SETTINGCHANGE: // 作業領域（タスクバーを除いた範囲）の変化。ディスプレイの構成が変わると届く
            if (wParam == SPI_SETWORKAREA)
                watchDisplayChange (hwnd, state);
            break;

        case WM_WINDOWPOSCHANGED: // 構成が変わってから見張っている間に動かされたら、止まるのを待ってから直す
            if (! state.nudging && state.watchUntilMs != 0)
            {
                if ((LONG) (GetTickCount() - state.watchUntilMs) < 0)
                    SetTimer (hwnd, nudgeTimerId, nudgeDelayMs, nullptr); // 待ち直す
                else
                    state.watchUntilMs = 0;
            }
            break;

        case WM_TIMER:
            if (wParam == nudgeTimerId)
            {
                KillTimer (hwnd, nudgeTimerId);
                nudge (hwnd, state);
                return 0;
            }
            break;

        case WM_SIZING: // 枠のドラッグでサイズを変え始めた（移動だけのときは来ない）
            setEngine (hwnd, softwareEngine);
            break;

        case WM_EXITSIZEMOVE: // ドラッグが終わった。サイズ変更のモーダルループを抜けてから戻す
            if (auto* peer = findPeer (hwnd); peer != nullptr && peer->getCurrentRenderingEngine() == softwareEngine)
                PostMessage (hwnd, restoreMessage, 0, 0);
            break;

        case restoreMessage:
        {
            setEngine (hwnd, direct2DEngine);

            // 枠のドラッグでサイズを変えた後は、Direct2D に戻しても中身がタイトルバーの高さ分ずれたままになるので描き直させる
            nudge (hwnd, state);
            return 0;
        }

        case WM_NCDESTROY:
            KillTimer (hwnd, nudgeTimerId);
            RemoveWindowSubclass (hwnd, windowProc, subclassId);
            delete &state;
            break;

        default:
            break;
    }

    return DefSubclassProc (hwnd, message, wParam, lParam);
}
} // namespace

namespace WindowRendering
{
void install (juce::Component& topLevelWindow)
{
    auto* peer = topLevelWindow.getPeer();
    if (peer == nullptr)
        return;

    // Direct2D が使えない環境では、描画方式は 1 つ（ソフトウェア）だけなので何もしない
    if (peer->getAvailableRenderingEngines().size() <= direct2DEngine)
        return;

    peer->setCurrentRenderingEngine (direct2DEngine);

    const auto hwnd = (HWND) peer->getNativeHandle();
    DWORD_PTR existing = 0;
    if (! GetWindowSubclass (hwnd, windowProc, subclassId, &existing)) // 2 回目以降は付け直さない（状態は WM_NCDESTROY で消す）
        SetWindowSubclass (hwnd, windowProc, subclassId, reinterpret_cast<DWORD_PTR> (new WindowState()));
}
} // namespace WindowRendering
