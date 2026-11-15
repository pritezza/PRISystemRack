#include "AppTray.h"

#include "ChainView.h"
#include "RackEngine.h"

namespace
{
// 状態の色（今までのアイコンの背景。メーターのアイコンには使わない）
const juce::Colour normalColour { 0xff737a86 };  // 処理中（目立たない灰）
const juce::Colour bypassColour { 0xffffd23f };  // バイパス中（黄）
const juce::Colour errorColour { 0xffff5a4f };   // エラー（赤）
// 注意もエラーと同じ赤にする（注意は回数が増えてから 3 秒だけ、エラーは読み込み直すまで出続けるので、出ている長さで見分けられる。
// 詳しくはマウスを乗せたときの説明に出す）。以前は紫だったが、暗いタスクバーの上で見えにくかった
const juce::Colour warningColour = errorColour;

// メーターの色は、クラシックな CPU メーター（利用者が見せた参考の画像）と同じにする（2026-11-12）:
// 消えている部分は暗い緑、ピークのバーは明るい緑、その上に下から重ねる RMS のバーは赤。
// （以前は溝が暗い灰で、ピークが水色・RMS がピンク、そのあと黄緑・青にしていた）
const juce::Colour trackColour { 0xff008000 };
const juce::Colour peakColour { 0xff00ff00 };
const juce::Colour rmsColour { 0xffff0000 };
const juce::Colour clipColour { 0xffff5a4f };  // 0 dBFS に達した印（メーターの上端）

// メーターの下端（上端は 0 dBFS）。16 px のアイコンではメーターの長さが 12 ドットなので、1 ドット約 3.3 dB。
// バーは平均（VU）なので、ピークより低いところで動く。-60 dB からだと 1 ドット 5 dB になり、上の方の動きが見えにくかった
constexpr float floorDb = -40.0f;
// メイン画面の「ピーク + RMS」と同じく、ピークのバーの上に RMS のバーを重ねる。
// - ピーク: 新しい値がなければ 40 dB/秒で下げる（全体を 1 秒。メイン画面のピークメーターと同じ。120 dB/秒では速すぎた）
// - RMS: 左右を合わせた RMS を、上がるときも下がるときも 300 ms で 99% に届く速さ（時定数 約 65 ms）でならす（VU のような動き）
// ピークホールドの白い線と、0 dBFS に達した印も出す
constexpr float peakFallDbPerSecond = 40.0f;
const double vuTimeConstantSeconds = 0.3 / std::log (100.0);
constexpr double peakHoldMs = 1500.0;      // ピークホールドの線を保つ時間（入力・出力の欄のピークホールドと同じ）
constexpr float holdFallDbPerSecond = 20.0f; // 保ったあとに線が下がる速さ（同上）
const juce::Colour holdColour { 0xfff3f4f6 }; // ピークホールドの線
constexpr double clipHoldMs = 1500.0;      // 0 dBFS に達した印を出しておく時間
constexpr double warningHoldMs = 3000.0;   // 注意の色にしておく時間（回数が最後に増えてから）
constexpr int stillHz = 4;                 // メーターを描かないときに状態を見る回数（1 秒あたり）

// メーターのアイコンの形: アイコン全体を左右 2 本のメーター（幅は大きさの半分ずつ）で使い、それぞれをくぼんだ縁で囲む
// （上と左が黒、下と右が明るい灰。クラシックな CPU メーターのような見た目）。隣り合う縁がそのまま 2 本の境目になる。
// 16 px で 横は 縁 1・メーター 6・縁 1 | 縁 1・メーター 6・縁 1、縦は 縁 1・メーター 14・縁 1。
// 状態（バイパス・注意・エラー）は枠の色では出さず、そのあいだは状態の色の静止画のアイコンに切り替える（2026-11-12 に変更。
// 以前は角丸の枠と仕切りを状態の色で描いていたので、メーターは 5 × 12 ドットしかなかった）
const juce::Colour edgeDarkColour { 0xff000000 };   // 縁の上と左
const juce::Colour edgeLightColour { 0xffffffff };  // 縁の下と右
const juce::Colour edgeCornerColour { 0xffc0c0c0 }; // 明るい縁と暗い縁が出会う角（右上と左下）

int borderFor (int size)
{
    return juce::jmax (1, juce::roundToInt ((float) size / 16.0f));
}

// メーターの長さ（縦のドット数）
int meterLength (int size)
{
    return size - 2 * borderFor (size);
}

int levelToPixels (float db, int length)
{
    if (db <= floorDb)
        return 0;
    const float fraction = juce::jlimit (0.0f, 1.0f, (db - floorDb) / -floorDb);
    return juce::roundToInt (fraction * (float) length);
}

// 今までのアイコン（Resources/Icons の PNG）
juce::Image stillIcon()
{
    // 表示の倍率が 150% 以上なら 32 px、それより小さければ 16 px の絵（どちらも小さいサイズ向けの簡略版）
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const bool large = display != nullptr && display->scale >= 1.5;
    return large ? juce::ImageCache::getFromMemory (BinaryData::icon32_png, BinaryData::icon32_pngSize)
                 : juce::ImageCache::getFromMemory (BinaryData::icon16_png, BinaryData::icon16_pngSize);
}
} // namespace

AppTrayIcon::AppTrayIcon (RackEngine& engineToUse)
    : engine (engineToUse)
{
    showImage (stillIcon());
    shownTooltip = "PRI System Rack";
    setIconTooltip (shownTooltip);
    startTimerHz (stillHz);
}

AppTrayIcon::~AppTrayIcon()
{
    stopTimer();
}

// アイコンの大きさ（ドット）。通知領域のアイコンは 16 px に表示の倍率をかけた大きさで出る
int AppTrayIcon::iconSize()
{
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const double scale = display != nullptr ? display->scale : 1.0;
    return juce::jlimit (16, 64, juce::roundToInt (16.0 * scale));
}

AppTrayIcon::State AppTrayIcon::currentState (double nowMs)
{
    // 待機: 有効にしてあるのに、再生デバイスが接続されていない、または APO とつながっていない（ステータスバーが注意の色のとき）。
    // デバイスが外れると［有効］も下がるので、下の「処理していない」より先に見る
    waitingReason = describeWaiting != nullptr ? describeWaiting() : juce::String();
    if (waitingReason.isNotEmpty())
        return State::waiting;

    // 処理していない: デバイスを選んでいない、デバイスで無効、プラグインを読み込み中、APO とつながっていない
    if (engine.getEndpointGuid().isEmpty() || ! PluginChain::isDeviceEnabled() || engine.isLoadingPlugins())
        return State::off;

    const auto stats = engine.getLinkStats();
    if (! stats.connected)
        return State::off;

    // 注意: 時間切れ・バッファ不足・不正な値の回数（累計）が前回より増えていたら、しばらく注意の色にする（最初の 1 回は今の値を覚えるだけ）。
    // 出力の保護（リミッター）が働いたことは含めない（音が大きければ普通に働くもので、異常ではないため）
    const juce::uint64 timeouts = stats.apoTimedOut, underruns = engine.getBufferUnderruns(),
                       invalid = engine.getOutputGuard().getInvalidCount();
    if (countersPrimed && (timeouts > lastTimeouts || underruns > lastUnderruns || invalid > lastInvalid))
        warningUntilMs = nowMs + warningHoldMs;
    countersPrimed = true;
    lastTimeouts = timeouts;
    lastUnderruns = underruns;
    lastInvalid = invalid;

    // エラー: エラーで止まったプラグインがある（メインのラックとアプリ用ラック）
    auto hasFault = [] (PluginChain& chain)
    {
        for (const auto& plugin : chain.getPlugins())
            if (plugin->faulted.load())
                return true;
        return false;
    };
    bool fault = hasFault (engine.getChain());
    for (const auto& rack : engine.getAppRacks())
        fault = fault || hasFault (rack->getChain());

    if (fault)
        return State::error;
    if (nowMs < warningUntilMs)
        return State::warning;
    if (engine.getChain().isBypassAll())
        return State::bypass;
    return State::normal;
}

juce::String AppTrayIcon::describeState (State state) const
{
    switch (state)
    {
        case State::normal:  return TRANS ("Processing");
        case State::bypass:  return TRANS ("Bypass is on");
        case State::warning: return TRANS ("Timeouts, buffer underruns or invalid samples occurred");
        case State::error:   return TRANS ("A plugin was stopped by an error");
        case State::waiting: return waitingReason;
        case State::off:     break;
    }
    return TRANS ("Not processing");
}

void AppTrayIcon::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double elapsedSeconds = lastTickMs > 0.0 ? juce::jlimit (0.0, 1.0, (nowMs - lastTickMs) / 1000.0) : 0.0;
    lastTickMs = nowMs;

    // メーターを描くときはメーターの fps で、それ以外は状態を見るだけなのでゆっくり
    const bool meters = style == Style::metersAndStatus;
    const int hz = meters ? ChainView::getMeterFramesPerSecond() : stillHz;
    if (getTimerInterval() != 1000 / hz)
        startTimer (1000 / hz);

    // 値は毎回読む（読まないとたまって、次に描くときに古い値が出る）
    const auto inLevel = engine.takeInputIconLevel();
    const auto outLevel = engine.takeOutputIconLevel();
    const float inPeak = inLevel.peak, outPeak = outLevel.peak;
    const float vuAmount = (float) (1.0 - std::exp (-elapsedSeconds / vuTimeConstantSeconds));
    auto follow = [elapsedSeconds, nowMs, vuAmount] (float& peakDb, float& vu, float& holdDb, double& holdUntilMs, LevelMeterTap::IconLevel level)
    {
        // ピークのバー: 新しい値がなければ決まった速さで下げる
        const float db = level.peak > 0.0f ? juce::Decibels::gainToDecibels (level.peak, -100.0f) : -100.0f;
        peakDb = juce::jmax (db, peakDb - peakFallDbPerSecond * (float) elapsedSeconds);

        // RMS のバー: VU の速さでならす（上がるときも下がるときも同じ速さ）
        vu += (level.rms - vu) * vuAmount;
        if (vu < 1.0e-6f) // -120 dB 未満は 0（無音で限りなく小さい値を持ち続けないように）
            vu = 0.0f;

        // ピークホールド: 最大の位置を 1.5 秒保ってから、ゆっくり下げる
        if (db >= holdDb)
        {
            holdDb = db;
            holdUntilMs = nowMs + peakHoldMs;
        }
        else if (nowMs >= holdUntilMs)
        {
            holdDb = juce::jmax (-100.0f, holdDb - holdFallDbPerSecond * (float) elapsedSeconds);
        }
    };
    follow (inDb, inVu, inHoldDb, inHoldUntilMs, inLevel);
    follow (outDb, outVu, outHoldDb, outHoldUntilMs, outLevel);
    if (inPeak >= 1.0f)
        inClipUntilMs = nowMs + clipHoldMs;
    if (outPeak >= 1.0f)
        outClipUntilMs = nowMs + clipHoldMs;

    const State state = currentState (nowMs);

    Look look;
    look.state = state;
    if (state != State::off)
    {
        look.size = iconSize();
        // メーターを出すのは処理中のときだけ。バイパス・注意・エラーのあいだは状態の色の静止画のアイコンにし、戻ったらメーターに戻す
        look.meters = meters && state == State::normal;
        if (look.meters)
        {
            const int length = meterLength (look.size);
            // RMS がピークのバーより上に出たら（ならし方が違うので、音が急に止まったときなど）、ピークのバーをそこまで伸ばす
            look.inRmsPixels = levelToPixels (juce::Decibels::gainToDecibels (inVu, -100.0f), length);
            look.outRmsPixels = levelToPixels (juce::Decibels::gainToDecibels (outVu, -100.0f), length);
            look.inPixels = juce::jmax (look.inRmsPixels, levelToPixels (inDb, length));
            look.outPixels = juce::jmax (look.outRmsPixels, levelToPixels (outDb, length));
            look.inHoldPixels = levelToPixels (inHoldDb, length);
            look.outHoldPixels = levelToPixels (outHoldDb, length);
            look.inClip = nowMs < inClipUntilMs;
            look.outClip = nowMs < outClipUntilMs;
        }
    }

    // 前と同じ絵なら差し替えない（差し替えのたびにエクスプローラーが描き直すため）
    if (! (shownLook.has_value() && *shownLook == look))
    {
        showImage (makeImage (look));
        shownLook = look;
    }

    // 処理が間に合っていない（RackEngine::checkOverload）: 説明をそれにし、始まったときに一度 Windows の通知で知らせる。
    // 画面を前面で見ているときは画面の赤い帯で分かるので知らせない。続けて出ないよう、知らせたら 10 分は知らせない
    const auto& overload = engine.checkOverload();
    const bool overloaded = state != State::off && state != State::waiting && overload.active;
    if (overloaded && ! wasOverloaded && (isWindowInFront == nullptr || ! isWindowInFront())
        && (lastOverloadNoticeMs == 0.0 || nowMs - lastOverloadNoticeMs >= overloadNoticeIntervalMs))
    {
        lastOverloadNoticeMs = nowMs;
        showInfoBubble (overload.describeTitle(), overload.describeDetail());
    }
    wasOverloaded = overloaded;

    // マウスを乗せたときの説明に今の状態を出す（変わったときだけ）
    const auto tooltip = "PRI System Rack - " + (overloaded ? overload.describeTitle() : describeState (state));
    if (tooltip != shownTooltip)
    {
        shownTooltip = tooltip;
        setIconTooltip (tooltip);
    }
}

void AppTrayIcon::showImage (const juce::Image& image)
{
    shownImage = image;
    setIconImage (image, image);
}

juce::Image AppTrayIcon::makeImage (const Look& look)
{
    // 処理していないとき、「動かさない」で処理中のときは、今までのアイコンのまま
    if (look.state == State::off || (! look.meters && look.state == State::normal))
        return stillIcon();
    return look.meters ? paintMeterIcon (look) : paintStillIcon (look.size, look.state);
}

namespace
{
juce::Colour stateColour (int state) // AppTrayIcon::State の値
{
    switch (state)
    {
        case 2:  return bypassColour;
        case 3:  return warningColour;
        case 4:  return errorColour;
        case 5:  return bypassColour; // 待機（ステータスバーでもバイパスと同じ注意の色）
        default: return normalColour;
    }
}
} // namespace

// アイコン全体を左右 2 本のメーターにする。それぞれくぼんだ縁で囲み、中は溝の色で塗って下からメーターを描く
juce::Image AppTrayIcon::paintMeterIcon (const Look& look)
{
    const int size = look.size;
    const int border = borderFor (size);
    const int length = meterLength (size);
    const int holdThickness = juce::jmax (1, size / 16); // ピークホールドの線の太さ（16 px で 1 ドット）

    juce::Image image (juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
    juce::Graphics g (image);

    auto drawMeter = [&] (int x0, int width, int pixels, int rmsPixels, int holdPixels, bool clip)
    {
        // 縁: 上と左を暗く、下と右を明るく（くぼんで見える）。明るい縁と暗い縁が出会う角は中間の灰
        g.setColour (edgeDarkColour);
        g.fillRect (x0, 0, width, border);
        g.fillRect (x0, 0, border, size);
        g.setColour (edgeLightColour);
        g.fillRect (x0, size - border, width, border);
        g.fillRect (x0 + width - border, 0, border, size);
        g.setColour (edgeCornerColour);
        g.fillRect (x0 + width - border, 0, border, border);
        g.fillRect (x0, size - border, border, border);

        const int x = x0 + border, top = border, barWidth = width - 2 * border;
        g.setColour (trackColour);
        g.fillRect (x, top, barWidth, length);
        g.setColour (peakColour);
        g.fillRect (x, top + length - pixels, barWidth, pixels);
        g.setColour (rmsColour);
        g.fillRect (x, top + length - rmsPixels, barWidth, rmsPixels);

        // ピークホールドの線（バーより上にあるときだけ。バーの先端と重なるときは描かない）
        if (holdPixels > pixels)
        {
            g.setColour (holdColour);
            g.fillRect (x, top + length - holdPixels, barWidth, holdThickness);
        }
        if (clip)
        {
            g.setColour (clipColour);
            g.fillRect (x, top, barWidth, juce::jmax (2, length / 6));
        }
    };

    const int leftWidth = size / 2;
    drawMeter (0, leftWidth, look.inPixels, look.inRmsPixels, look.inHoldPixels, look.inClip);
    drawMeter (leftWidth, size - leftWidth, look.outPixels, look.outRmsPixels, look.outHoldPixels, look.outClip);
    return image;
}

// 今までのアイコン（Tools/MakeIcon.ps1 の 32 px 以下の簡略版: 角丸の四角にラック 3 段、ランプと明るいバー）を、
// 背景と縁を状態の色にして描く
juce::Image AppTrayIcon::paintStillIcon (int size, State state)
{
    const float s = (float) size / 256.0f;
    const auto base = stateColour ((int) state);

    juce::Image image (juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
    juce::Graphics g (image);

    // 背景（上が明るいグラデーション）と縁
    juce::Path background;
    background.addRoundedRectangle (0.5f, 0.5f, (float) size - 1.0f, (float) size - 1.0f, 48.0f * s);
    g.setGradientFill (juce::ColourGradient (base.withMultipliedBrightness (1.05f), 0.0f, 0.0f,
                                             base.withMultipliedBrightness (0.62f), 0.0f, (float) size, false));
    g.fillPath (background);
    g.setColour (base.withMultipliedBrightness (1.25f));
    g.strokePath (background, juce::PathStrokeType (juce::jmax (1.0f, 5.0f * s)));

    // ラック 3 段（ランプの色は今のアイコンと同じ）
    const juce::Colour leds[] = { juce::Colour (0xff8fe3aa), juce::Colour (0xff5ccbfa), juce::Colour (0xff9584e0) };
    const float meters[] = { 0.78f, 0.55f, 0.9f };
    const float unitH = 62.0f * s, gap = 10.0f * s, left = 26.0f * s, right = 230.0f * s;
    const float top = ((float) size - (3.0f * unitH + 2.0f * gap)) / 2.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float y = top + (float) i * (unitH + gap);
        g.setColour (juce::Colour (0xff5d6470));
        g.fillRoundedRectangle (left, y, right - left, unitH, 10.0f * s);

        const float ledR = 19.0f * s, cx = left + 30.0f * s, cy = y + unitH / 2.0f;
        g.setColour (leds[i]);
        g.fillEllipse (cx - ledR, cy - ledR, 2.0f * ledR, 2.0f * ledR);

        const float bx = left + 58.0f * s, bw = (right - 14.0f * s) - bx, bh = 20.0f * s;
        g.setColour (juce::Colour (0xffd6dae0));
        g.fillRect (bx, cy - bh / 2.0f, bw * meters[i], bh);
    }
    return image;
}

void AppTrayIcon::writeSamples (const juce::File& folder)
{
    folder.createDirectory();
    auto write = [&folder] (const juce::Image& image, const juce::String& name)
    {
        const auto file = folder.getChildFile (name + ".png");
        file.deleteFile();
        if (juce::FileOutputStream out (file); out.openedOk())
            juce::PNGImageFormat().writeImageToStream (image, out);
    };

    const std::pair<State, const char*> states[] = {
        { State::normal, "1-normal" }, { State::bypass, "2-bypass" }, { State::warning, "3-warning" }, { State::error, "4-error" },
        { State::waiting, "5-waiting" }
    };
    for (const int size : { 16, 20, 24, 32 })
    {
        // メーター（処理中のときだけ出す）
        const int length = meterLength (size);
        Look look;
        look.state = State::normal;
        look.size = size;
        look.meters = true;
        look.inPixels = juce::roundToInt ((float) length * 0.6f);
        look.outPixels = juce::roundToInt ((float) length * 0.75f);
        look.inRmsPixels = juce::roundToInt ((float) length * 0.35f);
        look.outRmsPixels = juce::roundToInt ((float) length * 0.5f);
        look.inHoldPixels = juce::roundToInt ((float) length * 0.8f);
        look.outHoldPixels = juce::roundToInt ((float) length * 0.9f);
        write (makeImage (look), "meters-" + juce::String (size));

        look.inPixels = juce::roundToInt ((float) length * 0.75f);
        look.inRmsPixels = juce::roundToInt ((float) length * 0.45f);
        look.inHoldPixels = juce::roundToInt ((float) length * 0.9f);
        look.outPixels = length;
        look.outRmsPixels = juce::roundToInt ((float) length * 0.7f);
        look.outHoldPixels = length;
        look.outClip = true;
        write (makeImage (look), "clip-" + juce::String (size));

        // 静止画（処理中は今までのアイコン、ほかの状態はその色の背景。メーターの設定でも、状態を知らせるときはこれを出す）
        for (const auto& [state, name] : states)
        {
            Look still;
            still.state = state;
            still.size = size;
            write (makeImage (still), "still-" + juce::String (size) + "-" + name);
        }
    }
}
