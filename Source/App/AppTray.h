#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <optional>

#include "BinaryData.h"
#include "Theme.h"

// Windows の起動時（ログオン時）の自動起動。現在のユーザーのレジストリの Run に「"<exe>" --tray」を書く（管理者の許可は要らない）。
// --tray で起動すると、ウィンドウを出さずに通知領域のアイコンだけを出す
namespace AutoStart
{
inline const juce::String registryValue = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\PRISystemRack";

inline juce::String command()
{
    return "\"" + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName() + "\" --tray";
}

inline bool isEnabled() { return juce::WindowsRegistry::valueExists (registryValue); }

inline void setEnabled (bool shouldBeEnabled)
{
    if (shouldBeEnabled)
        juce::WindowsRegistry::setValue (registryValue, command());
    else
        juce::WindowsRegistry::deleteValue (registryValue);
}

// 登録してあれば、今の exe の場所に書き直す（フォルダーを移したときなど）
inline void refreshPath()
{
    if (isEnabled() && juce::WindowsRegistry::getValue (registryValue) != command())
        setEnabled (true);
}
} // namespace AutoStart

//==============================================================================
// 通知領域のメニューの見た目。画面の中の部品から開くメニューは JUCE が元の部品の倍率（表示の大きさ）に合わせて拡大するが、
// 通知領域から開くメニューには元の部品がないので、文字と行の高さにここで表示の大きさ（Theme::effectiveScale）をかける
class TrayMenuLookAndFeel final : public AppLookAndFeel
{
public:
    juce::Font getPopupMenuFont() override
    {
        return juce::FontOptions (Theme::popupTextSize * Theme::effectiveScale());
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override
    {
        AppLookAndFeel::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
        if (! isSeparator)
            idealHeight = juce::jmax (idealHeight, juce::roundToInt (24.0f * Theme::effectiveScale()));
    }
};

class RackEngine;

//==============================================================================
// 通知領域（タスクトレイ）のアイコン。アプリが動いている間は常に出す。
// 左クリックで画面を開き、右クリックでメニュー（画面を開く、バイパス、プリセット、終了）を出す。
// メニューの中身（バイパスとプリセット）は画面（MainComponent）が足す。
//
// アイコンの絵（設定メニューの「通知領域のアイコン」。設定の trayIcon）:
// - メーターと状態: アイコン全体を入力（左）・出力（右）の 2 本のメーターにする（くぼんだ縁で囲む。クラシックな CPU メーターと同じ色で、
//   暗い緑の溝に明るい緑のピークのバー、VU のようにならした RMS を赤で下から重ねる。白い線はピークホールド）。バイパス・注意・エラー・待機のあいだは、下の静止画（状態の色の背景）に
//   切り替え、戻ったらメーターに戻す。メーターの fps（表示メニュー）の回数だけ描き直すが、前と同じ絵になるときは差し替えない（無音のときは差し替えない）
// - 静止画（既定）: 今までのアイコン（ラック 3 段）。バイパス・注意・エラー・待機のときは背景（と縁）を状態の色にする。状態が変わったときだけ差し替える
// 待機（黄）: 有効にしてあるのに、再生デバイスが接続されていない、または APO とつながっていない（ステータスバーが注意の色のとき）
// 処理していないとき（デバイスを選んでいない、再生デバイスで無効、プラグインを読み込み中、切り替え中）は、どの設定でも今までのアイコンにする
class AppTrayIcon final : public juce::SystemTrayIconComponent,
                          private juce::Timer
{
public:
    // 値は設定の trayIcon（1 は以前の「状態だけ」なので使わない）
    enum class Style { metersAndStatus = 0, still = 2 };
    static void setStyle (Style newStyle) { style = newStyle; } // 次のタイマーで反映する
    static Style getStyle() { return style; }

    // 確認用: すべての状態と大きさの絵を folder に PNG で書く（起動オプション --dump-tray-samples）
    static void writeSamples (const juce::File& folder);

    std::function<void()> onOpen;                       // 画面を開く
    std::function<void (juce::PopupMenu&)> addMenuItems; // 「画面を開く」と「終了」の間に足す項目
    std::function<void()> onQuit;
    std::function<bool()> isWindowInFront; // 画面が前面にあるか（処理が間に合っていないときの通知を出すか決める）
    // 有効にしてあるのに処理できず待っている理由（MainComponent::describeWaiting）。空でなければ待っている（黄の静止画にする）
    std::function<juce::String()> describeWaiting;

    explicit AppTrayIcon (RackEngine& engineToUse);
    ~AppTrayIcon() override;

    // 今出している絵（確認用。起動オプション --dump-tray-icon で PNG に書く）
    juce::Image getShownImage() const { return shownImage; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showMenu();
        else if (onOpen != nullptr)
            onOpen();
    }

private:
    void showMenu()
    {
        juce::PopupMenu menu;
        menu.setLookAndFeel (&menuLookAndFeel); // 表示の大きさに合わせる（サブメニューにも効く）
        menu.addItem (TRANS ("Open PRI System Rack"), [this] { if (onOpen != nullptr) onOpen(); });
        menu.addSeparator();
        if (addMenuItems != nullptr)
        {
            addMenuItems (menu);
            menu.addSeparator();
        }
        menu.addItem (TRANS ("Exit"), [this] { if (onQuit != nullptr) onQuit(); });

        // 通知領域のメニューは、先にこのアプリを前面にしないと、ほかの場所をクリックしても閉じない（Windows の決まり）
        juce::Process::makeForegroundProcess();
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    // 状態（静止画の背景の色）。off は処理していない（今までのアイコンにする）。
    // waiting は有効にしてあるのに処理できず待っている（再生デバイスが接続されていない、APO を待っている。バイパスと同じ黄）
    enum class State { off, normal, bypass, warning, error, waiting };

    // 前に差し替えた絵と同じなら差し替えない
    struct Look
    {
        State state = State::off;
        bool meters = false; // false なら今までのアイコン（状態の色の背景）
        int size = 0;
        int inPixels = 0, outPixels = 0;         // ピークのバーの高さ（明るい緑）
        int inRmsPixels = 0, outRmsPixels = 0;   // RMS のバーの高さ（赤。ピークのバーの上に下から重ねる）
        int inHoldPixels = 0, outHoldPixels = 0; // ピークホールドの線の高さ
        bool inClip = false, outClip = false;
        bool operator== (const Look& other) const
        {
            return state == other.state && meters == other.meters && size == other.size && inPixels == other.inPixels
                   && outPixels == other.outPixels && inRmsPixels == other.inRmsPixels && outRmsPixels == other.outRmsPixels
                   && inHoldPixels == other.inHoldPixels && outHoldPixels == other.outHoldPixels
                   && inClip == other.inClip && outClip == other.outClip;
        }
    };

    void timerCallback() override;
    State currentState (double nowMs);
    static int iconSize();
    static juce::Image makeImage (const Look& look);
    static juce::Image paintMeterIcon (const Look& look);
    static juce::Image paintStillIcon (int size, State state);
    juce::String describeState (State state) const;

    inline static Style style = Style::still;

    RackEngine& engine;
    TrayMenuLookAndFeel menuLookAndFeel;

    void showImage (const juce::Image& image);

    std::optional<Look> shownLook; // 今出している絵（まだなら空）
    juce::Image shownImage;
    juce::String shownTooltip;
    juce::String waitingReason; // 待っている理由（State::waiting のときの説明）
    double lastTickMs = 0.0;
    float inDb = -100.0f, outDb = -100.0f;        // ピークのバーの値（下がるときは決まった速さで）
    float inVu = 0.0f, outVu = 0.0f;               // RMS のバーの値（RMS を VU の速さでならしたもの。振幅）
    float inHoldDb = -100.0f, outHoldDb = -100.0f; // ピークホールドの値
    double inHoldUntilMs = 0.0, outHoldUntilMs = 0.0;
    double inClipUntilMs = 0.0, outClipUntilMs = 0.0; // 0 dBFS に達したら、しばらく先端を赤くする

    // 注意: 時間切れ・バッファ不足・不正な値の回数（累計）が増えたら、しばらく注意の色（エラーと同じ赤）にする
    bool countersPrimed = false;
    juce::uint64 lastTimeouts = 0, lastUnderruns = 0, lastInvalid = 0;
    double warningUntilMs = 0.0;

    // 処理が間に合っていないときの Windows の通知（始まったときに一度。間を 10 分あける）
    static constexpr double overloadNoticeIntervalMs = 10.0 * 60.0 * 1000.0;
    bool wasOverloaded = false;
    double lastOverloadNoticeMs = 0.0;
};
