#include "PluginActivityView.h"

#include "Theme.h"

namespace
{
const juce::Colour cutColour = Theme::accent;    // 削っている（出力 < 入力）
const juce::Colour boostColour = Theme::warning; // 足している（出力 > 入力）

constexpr float silentRms = 1.0e-4f; // これより小さければ音がないとみなす（-80 dB）
constexpr float lowFrequency = 40.0f;
constexpr float highFrequency = 18000.0f;

float bandEdge (int index, int count)
{
    return lowFrequency * std::pow (highFrequency / lowFrequency, (float) index / (float) count);
}

// 周波数の位置（0〜1。対数）
float frequencyPosition (float hz)
{
    return std::log (hz / lowFrequency) / std::log (highFrequency / lowFrequency);
}

float rms (const float* data, int count)
{
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
        sum += (double) data[i] * data[i];
    return (float) std::sqrt (sum / count);
}

// 下の値は 20 fps のときの 1 回分で決めてあるので、今の fps の 1 回分に直す（時間あたりの動きを fps によらずそろえる）
float ticksAt20()
{
    return 20.0f / (float) PluginActivityView::getFramesPerSecond();
}

// 1 回で残す割合（20 fps のとき keep）
float keepPerTick (float keep)
{
    return std::pow (keep, ticksAt20());
}

// 1 回で近づく割合（20 fps のとき amount）
float approachPerTick (float amount)
{
    return 1.0f - keepPerTick (1.0f - amount);
}
} // namespace

void PluginActivityView::setFramesPerSecond (int fps)
{
    framesPerSecond = juce::jlimit (fpsChoices[0], fpsChoices[std::size (fpsChoices) - 1], fps);
}

PluginActivityView::PluginActivityView()
    : inL ((size_t) fftSize), inR ((size_t) fftSize), outL ((size_t) fftSize), outR ((size_t) fftSize),
      inMono ((size_t) fftSize), outMono ((size_t) fftSize), fftData ((size_t) fftSize * 2), window ((size_t) fftSize)
{
    setInterceptsMouseClicks (false, false);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), (size_t) fftSize,
                                                              juce::dsp::WindowingFunction<float>::hann, false);
    resetDisplay();
}

juce::String PluginActivityView::viewName (View view)
{
    switch (view)
    {
        case View::none:         return TRANS ("None");
        case View::deltaBar:     return TRANS ("Gain change bar");
        case View::levelBars:    return TRANS ("Input and output levels");
        case View::deltaHistory: return TRANS ("Gain change history");
        case View::spectrum:     return TRANS ("Gain change by frequency");
        case View::outputSpectrum: return TRANS ("Output spectrum");
        case View::cpuLoad:      return TRANS ("Processing load (CPU)");
        case View::correlation:  return TRANS ("Phase correlation");
        case View::goniometer:   return TRANS ("Goniometer");
        default:                 return {};
    }
}

const std::vector<PluginActivityView::View>& PluginActivityView::viewOrder()
{
    static const std::vector<View> order { View::deltaBar, View::levelBars, View::deltaHistory, View::spectrum, View::outputSpectrum,
                                           View::correlation, View::goniometer, View::cpuLoad, View::none };
    return order;
}

PluginActivityView::View PluginActivityView::nextView (View view, int steps)
{
    const auto& order = viewOrder();
    const int count = (int) order.size();

    int index = 0;
    for (int i = 0; i < count; ++i)
        if (order[i] == view)
            index = i;

    return order[((index + steps) % count + count) % count];
}

void PluginActivityView::resetDisplay()
{
    inDb = outDb = -100.0f;
    deltaDb = 0.0f;
    deltaValid = false;
    history.assign ((size_t) historyLength(), std::numeric_limits<float>::quiet_NaN());
    bandIn.fill (0.0f);
    bandOut.fill (0.0f);
    bandDelta.fill (0.0f);
    bandValid.fill (false);
    analyserIn.fill (analyserFloorDb);
    analyserOut.fill (analyserFloorDb);
    loadAverage = loadPeak = 0.0f;
    correlationOut = correlationIn = 0.0f;
    correlationValid = false;
    goniometer.clear();
}

void PluginActivityView::update (const PluginChain::Plugin* plugin)
{
    const auto newView = plugin != nullptr ? plugin->view : View::none;
    if (newView != view)
    {
        view = newView;
        fresh = false;
        resetDisplay();
        repaint();
    }

    if (plugin == nullptr || view == View::none || ! isShowing())
        return;

    // OFF・休止中・止まっているときは処理していないので、表示を空にする
    if (! plugin->tap.isFresh())
    {
        if (fresh)
        {
            fresh = false;
            resetDisplay();
            repaint();
        }
        return;
    }

    fresh = true;

    // 処理の重さは音を取り出さずに読む（平均と、前に読んでからの最大。最大の表示はゆっくり下げる）
    if (view == View::cpuLoad)
    {
        loadAverage = plugin->tap.getLoadAverage();
        loadPeak = juce::jmax (plugin->tap.takeLoadPeak(), loadPeak * keepPerTick (0.97f));
        repaint();
        return;
    }

    // 出力の直近 fftSize サンプルと、レイテンシー分前の入力（時間を合わせたもの）
    if (! plugin->tap.read (inL.data(), inR.data(), outL.data(), outR.data(), fftSize, PluginChain::getLatencySamples (*plugin)))
        return;

    for (int i = 0; i < fftSize; ++i)
    {
        inMono[(size_t) i] = (inL[(size_t) i] + inR[(size_t) i]) * 0.5f;
        outMono[(size_t) i] = (outL[(size_t) i] + outR[(size_t) i]) * 0.5f;
    }

    const double sampleRate = (double) plugin->tap.getSampleRate();
    analyseLevels (inMono.data(), outMono.data());
    if (view == View::spectrum)
        analyseSpectrum (inMono.data(), outMono.data(), sampleRate);
    else if (view == View::outputSpectrum)
        analyseOutputSpectrum (inMono.data(), outMono.data(), sampleRate);
    else if (view == View::correlation || view == View::goniometer)
        analyseStereo();
    repaint();
}

void PluginActivityView::analyseLevels (const float* in, const float* out)
{
    const float rmsIn = rms (in, fftSize);
    const float rmsOut = rms (out, fftSize);
    const float newIn = juce::Decibels::gainToDecibels (rmsIn, -100.0f);
    const float newOut = juce::Decibels::gainToDecibels (rmsOut, -100.0f);

    // レベルは上がるときはすぐ、下がるときはゆっくり（30 dB/秒）
    const float fall = 1.5f * ticksAt20();
    inDb = juce::jmax (newIn, inDb - fall);
    outDb = juce::jmax (newOut, outDb - fall);

    deltaValid = rmsIn > silentRms || rmsOut > silentRms;
    if (deltaValid)
        deltaDb += approachPerTick (0.35f) * (juce::jlimit (-rangeDb * 2.0f, rangeDb * 2.0f, newOut - newIn) - deltaDb);
    else
        deltaDb *= keepPerTick (0.8f);

    if ((int) history.size() != historyLength()) // fps を変えたら、4 秒分の点の数で作り直す
        history.assign ((size_t) historyLength(), std::numeric_limits<float>::quiet_NaN());
    history.erase (history.begin());
    history.push_back (deltaValid ? deltaDb : std::numeric_limits<float>::quiet_NaN());
}

void PluginActivityView::analyseSpectrum (const float* in, const float* out, double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    std::array<float, bandCount> powers {};
    const float keep = keepPerTick (0.6f);
    bandPowers (in, bandCount, powers.data(), sampleRate);
    for (int band = 0; band < bandCount; ++band)
        bandIn[(size_t) band] = bandIn[(size_t) band] * keep + powers[(size_t) band] * (1.0f - keep);
    bandPowers (out, bandCount, powers.data(), sampleRate);
    for (int band = 0; band < bandCount; ++band)
        bandOut[(size_t) band] = bandOut[(size_t) band] * keep + powers[(size_t) band] * (1.0f - keep);

    // 全体の大きさのサイン波（Hann 窓）の強さはおよそ (fftSize / 4)^2。それより 85 dB 以上小さい帯域は音がないとみなす
    const float threshold = (float) (fftSize / 4) * (float) (fftSize / 4) * 3.0e-9f;
    for (int band = 0; band < bandCount; ++band)
    {
        const auto i = (size_t) band;
        bandValid[i] = juce::jmax (bandIn[i], bandOut[i]) > threshold;
        bandDelta[i] = bandValid[i] ? 10.0f * std::log10 ((bandOut[i] + 1.0e-12f) / (bandIn[i] + 1.0e-12f)) : 0.0f;
    }
}

void PluginActivityView::bandPowers (const float* source, int bands, float* powers, double sampleRate)
{
    const double binHz = sampleRate / fftSize;
    const int maxBin = fftSize / 2;

    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = source[i] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    for (int band = 0; band < bands; ++band)
    {
        const int lo = juce::jlimit (1, maxBin - 1, (int) std::floor (bandEdge (band, bands) / binHz));
        const int hi = juce::jlimit (lo + 1, maxBin, (int) std::ceil (bandEdge (band + 1, bands) / binHz));
        double power = 0.0;
        for (int bin = lo; bin < hi; ++bin)
            power += (double) fftData[(size_t) bin] * fftData[(size_t) bin];
        powers[band] = (float) (power / (hi - lo));
    }
}

void PluginActivityView::analyseOutputSpectrum (const float* in, const float* out, double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    // 帯域ごとの強さを dBFS にする（全体の大きさのサイン波（Hann 窓）の強さ (fftSize / 4)^2 を 0 dB とする）。
    // 上がるときはすぐ、下がるときはゆっくり（40 dB/秒）
    const float reference = (float) (fftSize / 4) * (float) (fftSize / 4);
    std::array<float, analyserBandCount> powers {};
    const float fall = 2.0f * ticksAt20();

    auto toDb = [&] (std::array<float, analyserBandCount>& display)
    {
        for (int band = 0; band < analyserBandCount; ++band)
        {
            const float db = juce::jmax (analyserFloorDb, 10.0f * std::log10 (powers[(size_t) band] / reference + 1.0e-12f));
            display[(size_t) band] = juce::jmax (db, display[(size_t) band] - fall);
        }
    };

    bandPowers (in, analyserBandCount, powers.data(), sampleRate);
    toDb (analyserIn);
    bandPowers (out, analyserBandCount, powers.data(), sampleRate);
    toDb (analyserOut);
}

void PluginActivityView::analyseStereo()
{
    // 位相相関: Σ LR / √(Σ L² Σ R²)。どちらかがほぼ無音なら出さない
    auto correlation = [] (const std::vector<float>& left, const std::vector<float>& right, bool& valid)
    {
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (size_t i = 0; i < left.size(); ++i)
        {
            lr += (double) left[i] * right[i];
            ll += (double) left[i] * left[i];
            rr += (double) right[i] * right[i];
        }
        const double minEnergy = (double) silentRms * silentRms * (double) left.size();
        valid = ll > minEnergy && rr > minEnergy;
        return valid ? (float) (lr / std::sqrt (ll * rr)) : 0.0f;
    };

    bool inValid = false;
    const float outValue = correlation (outL, outR, correlationValid);
    const float inValue = correlation (inL, inR, inValid);
    const float approach = approachPerTick (0.3f);
    correlationOut += approach * ((correlationValid ? outValue : 0.0f) - correlationOut);
    correlationIn += approach * ((inValid ? inValue : 0.0f) - correlationIn);

    // ゴニオメーター: 縦が中央（L+R）、横が左右の差（L-R）。一番大きい点が枠に収まるよう大きさをそろえる
    goniometer.clear();
    const int step = juce::jmax (1, fftSize / goniometerPoints);
    float peak = 1.0e-6f;
    for (int i = 0; i < fftSize; i += step)
    {
        const float side = (outL[(size_t) i] - outR[(size_t) i]) * 0.70710678f;
        const float mid = (outL[(size_t) i] + outR[(size_t) i]) * 0.70710678f;
        goniometer.push_back ({ side, mid });
        peak = juce::jmax (peak, std::abs (side), std::abs (mid));
    }

    if (peak < silentRms)
        goniometer.clear();
    else
        for (auto& point : goniometer)
            point /= peak;
}

//==============================================================================
juce::String PluginActivityView::formatCorrelation (float value)
{
    const float rounded = std::round (value * 100.0f) / 100.0f;
    return (rounded > 0.0f ? "+" : "") + juce::String (rounded, 2);
}

juce::String PluginActivityView::formatDelta (float db)
{
    const float rounded = std::round (db * 10.0f) / 10.0f;
    if (rounded == 0.0f)
        return "0.0 dB";
    return (rounded > 0.0f ? "+" : "") + juce::String (rounded, 1) + " dB";
}

void PluginActivityView::paint (juce::Graphics& g)
{
    if (view == View::none)
        return;

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Theme::rackBackground);
    g.fillRoundedRectangle (bounds, 5.0f);

    {
        const juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (bounds, 5.0f);
        g.reduceClipRegion (clip);

        switch (view)
        {
            case View::deltaBar:     paintDeltaBar (g, bounds); break;
            case View::levelBars:    paintLevelBars (g, bounds); break;
            case View::deltaHistory: paintHistory (g, bounds); break;
            case View::spectrum:     paintSpectrum (g, bounds); break;
            case View::outputSpectrum: paintOutputSpectrum (g, bounds); break;
            case View::cpuLoad:      paintLoad (g, bounds); break;
            case View::correlation:  paintCorrelation (g, bounds); break;
            case View::goniometer:   paintGoniometer (g, bounds); break;
            case View::none:
            default:                 break;
        }
    }

    g.setColour (Theme::cardBorder);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
}

// 増減バー: 上に中央が 0 dB のバー、下に数値
void PluginActivityView::paintDeltaBar (juce::Graphics& g, juce::Rectangle<float> area)
{
    auto inner = area.reduced (7.0f, 5.0f);
    const auto track = inner.removeFromTop (inner.getHeight() * 0.42f).withSizeKeepingCentre (inner.getWidth(), 8.0f);

    g.setColour (Theme::card);
    g.fillRoundedRectangle (track, 3.0f);

    const float centre = track.getCentreX();
    if (fresh && deltaValid)
    {
        const float proportion = juce::jlimit (-1.0f, 1.0f, deltaDb / rangeDb);
        const float width = std::abs (proportion) * track.getWidth() * 0.5f;
        g.setColour (proportion < 0.0f ? cutColour : boostColour);
        g.fillRect (proportion < 0.0f ? track.withLeft (centre - width).withRight (centre) : track.withLeft (centre).withWidth (width));
    }

    g.setColour (Theme::subText);
    g.fillRect (centre - 0.75f, track.getY() - 2.0f, 1.5f, track.getHeight() + 4.0f);

    g.setColour (! fresh || ! deltaValid ? Theme::dimText : (deltaDb < -0.05f ? cutColour : (deltaDb > 0.05f ? boostColour : Theme::subText)));
    g.setFont (juce::FontOptions (Theme::smallSize - 1.0f, juce::Font::bold));
    g.drawText (fresh && deltaValid ? formatDelta (deltaDb) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")), // —
                inner, juce::Justification::centred, false);
}

// 入力と出力のレベル: 横向きの 2 本のバー（上が入力、下が出力。-60〜0 dB）。幅があれば右に差の数値
void PluginActivityView::paintLevelBars (juce::Graphics& g, juce::Rectangle<float> area)
{
    auto inner = area.reduced (6.0f, 5.0f);
    const bool showText = inner.getWidth() >= 110.0f;
    const auto textArea = showText ? inner.removeFromRight (50.0f) : juce::Rectangle<float>();
    if (showText)
        inner.removeFromRight (4.0f);

    const juce::FontOptions labelFont (10.0f, juce::Font::bold);
    const float labelWidth = 24.0f;
    const float rowHeight = inner.getHeight() * 0.5f;

    auto drawRow = [&] (juce::Rectangle<float> row, const juce::String& label, float db, juce::Colour colour)
    {
        g.setColour (Theme::dimText);
        g.setFont (labelFont);
        g.drawText (label, row.removeFromLeft (labelWidth), juce::Justification::centredLeft, false);

        const auto track = row.withSizeKeepingCentre (row.getWidth(), 7.0f);
        g.setColour (Theme::card);
        g.fillRoundedRectangle (track, 2.5f);

        if (fresh)
        {
            const float proportion = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            g.setColour (db > -1.0f ? juce::Colours::red : colour);
            g.fillRoundedRectangle (track.withWidth (track.getWidth() * proportion), 2.5f);
        }
    };

    drawRow (inner.removeFromTop (rowHeight), "IN", inDb, Theme::subText);
    drawRow (inner, "OUT", outDb, Theme::accent);

    if (showText)
    {
        g.setColour (! fresh || ! deltaValid ? Theme::dimText : (deltaDb < -0.05f ? cutColour : (deltaDb > 0.05f ? boostColour : Theme::subText)));
        g.setFont (juce::FontOptions (Theme::smallSize - 1.0f, juce::Font::bold));
        g.drawText (fresh && deltaValid ? formatDelta (deltaDb) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")),
                    textArea, juce::Justification::centredRight, false);
    }
}

// 増減の履歴: 直近 4 秒（右が今）。中央の線が 0 dB、上は足している、下は削っている（±12 dB）
void PluginActivityView::paintHistory (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto inner = area.reduced (4.0f, 4.0f);
    const float zeroY = inner.getCentreY();
    const float half = inner.getHeight() * 0.5f;

    g.setColour (Theme::cardBorder);
    g.fillRect (inner.getX(), zeroY - 0.5f, inner.getWidth(), 1.0f);

    const int length = (int) history.size();
    if (length < 2)
        return;
    auto xAt = [&] (int i) { return inner.getX() + inner.getWidth() * (float) i / (float) (length - 1); };
    auto yAt = [&] (float db) { return zeroY - juce::jlimit (-1.0f, 1.0f, db / rangeDb) * half; };

    // 音のある区間ごとに、0 dB の線との間を塗って線を引く
    int i = 0;
    while (i < length)
    {
        if (std::isnan (history[(size_t) i]))
        {
            ++i;
            continue;
        }

        juce::Path line, fill;
        const int start = i;
        fill.startNewSubPath (xAt (start), zeroY);
        for (; i < length && ! std::isnan (history[(size_t) i]); ++i)
        {
            const auto point = juce::Point<float> (xAt (i), yAt (history[(size_t) i]));
            if (i == start)
                line.startNewSubPath (point);
            else
                line.lineTo (point);
            fill.lineTo (point);
        }
        fill.lineTo (xAt (i - 1), zeroY);
        fill.closeSubPath();

        {
            const juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (inner.withBottom (zeroY).toNearestInt());
            g.setColour (boostColour.withAlpha (0.35f));
            g.fillPath (fill);
        }
        {
            const juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (inner.withTop (zeroY).toNearestInt());
            g.setColour (cutColour.withAlpha (0.35f));
            g.fillPath (fill);
        }
        g.setColour (Theme::text.withAlpha (0.85f));
        g.strokePath (line, juce::PathStrokeType (1.3f));
    }
}

// 周波数ごとの増減: 左が低域、右が高域（40 Hz〜18 kHz、対数）。中央の線が 0 dB（±12 dB）
void PluginActivityView::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto inner = area.reduced (4.0f, 4.0f);
    const float zeroY = inner.getCentreY();
    const float half = inner.getHeight() * 0.5f;

    // 100 Hz、1 kHz、10 kHz の目安の線
    g.setColour (Theme::card);
    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
        g.fillRect (inner.getX() + inner.getWidth() * frequencyPosition (hz) - 0.5f, inner.getY(), 1.0f, inner.getHeight());

    g.setColour (Theme::cardBorder);
    g.fillRect (inner.getX(), zeroY - 0.5f, inner.getWidth(), 1.0f);

    if (! fresh)
        return;

    juce::Path line, fill;
    fill.startNewSubPath (inner.getX(), zeroY);
    for (int band = 0; band < bandCount; ++band)
    {
        const float x = inner.getX() + inner.getWidth() * ((float) band + 0.5f) / (float) bandCount;
        const float db = bandValid[(size_t) band] ? bandDelta[(size_t) band] : 0.0f;
        const juce::Point<float> point (x, zeroY - juce::jlimit (-1.0f, 1.0f, db / rangeDb) * half);
        if (band == 0)
        {
            fill.lineTo (inner.getX(), point.y);
            line.startNewSubPath (inner.getX(), point.y);
        }
        line.lineTo (point);
        fill.lineTo (point);
        if (band == bandCount - 1)
        {
            line.lineTo (inner.getRight(), point.y);
            fill.lineTo (inner.getRight(), point.y);
        }
    }
    fill.lineTo (inner.getRight(), zeroY);
    fill.closeSubPath();

    {
        const juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (inner.withBottom (zeroY).toNearestInt());
        g.setColour (boostColour.withAlpha (0.4f));
        g.fillPath (fill);
    }
    {
        const juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (inner.withTop (zeroY).toNearestInt());
        g.setColour (cutColour.withAlpha (0.4f));
        g.fillPath (fill);
    }
    g.setColour (Theme::text.withAlpha (0.85f));
    g.strokePath (line, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// 出力のスペクトル: 左が低域、右が高域（40 Hz〜18 kHz、対数）。縦は -90〜0 dBFS。出力を塗り、入力を薄い線で重ねる
void PluginActivityView::paintOutputSpectrum (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto inner = area.reduced (4.0f, 4.0f);

    g.setColour (Theme::card);
    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
        g.fillRect (inner.getX() + inner.getWidth() * frequencyPosition (hz) - 0.5f, inner.getY(), 1.0f, inner.getHeight());

    if (! fresh)
        return;

    auto pointAt = [&] (int band, const std::array<float, analyserBandCount>& values)
    {
        const float x = inner.getX() + inner.getWidth() * ((float) band + 0.5f) / (float) analyserBandCount;
        const float proportion = juce::jlimit (0.0f, 1.0f, (values[(size_t) band] - analyserFloorDb) / -analyserFloorDb);
        return juce::Point<float> (x, inner.getBottom() - proportion * inner.getHeight());
    };

    auto makeLine = [&] (const std::array<float, analyserBandCount>& values)
    {
        juce::Path line;
        line.startNewSubPath (inner.getX(), pointAt (0, values).y);
        for (int band = 0; band < analyserBandCount; ++band)
            line.lineTo (pointAt (band, values));
        line.lineTo (inner.getRight(), pointAt (analyserBandCount - 1, values).y);
        return line;
    };

    const auto outLine = makeLine (analyserOut);
    juce::Path fill (outLine);
    fill.lineTo (inner.getBottomRight());
    fill.lineTo (inner.getBottomLeft());
    fill.closeSubPath();

    g.setGradientFill (juce::ColourGradient::vertical (Theme::accent.withAlpha (0.55f), inner.getY(),
                                                       Theme::accent.withAlpha (0.12f), inner.getBottom()));
    g.fillPath (fill);
    g.setColour (Theme::accent);
    g.strokePath (outLine, juce::PathStrokeType (1.2f));

    // 入力（エフェクトに入る前）は赤い線。出力（水色）との差が分かりやすいように
    g.setColour (Theme::error.withAlpha (0.85f));
    g.strokePath (makeLine (analyserIn), juce::PathStrokeType (1.0f));
}

// 処理の重さ: 上に横バー（0〜100%。平均を塗り、最近の最大を縦線で）、下に数値（平均 / 最大）
void PluginActivityView::paintLoad (juce::Graphics& g, juce::Rectangle<float> area)
{
    auto inner = area.reduced (7.0f, 5.0f);
    const auto track = inner.removeFromTop (inner.getHeight() * 0.42f).withSizeKeepingCentre (inner.getWidth(), 8.0f);

    g.setColour (Theme::card);
    g.fillRoundedRectangle (track, 3.0f);

    auto colourFor = [] (float load) { return load >= 0.8f ? Theme::error : (load >= 0.5f ? Theme::warning : Theme::ok); };

    if (fresh)
    {
        const float average = juce::jlimit (0.0f, 1.0f, loadAverage);
        g.setColour (colourFor (loadAverage));
        g.fillRoundedRectangle (track.withWidth (juce::jmax (2.0f, track.getWidth() * average)), 3.0f);

        const float peakX = track.getX() + track.getWidth() * juce::jlimit (0.0f, 1.0f, loadPeak);
        g.setColour (colourFor (loadPeak));
        g.fillRect (peakX - 1.0f, track.getY() - 2.0f, 2.0f, track.getHeight() + 4.0f);
    }

    g.setColour (fresh ? Theme::subText : Theme::dimText);
    g.setFont (juce::FontOptions (Theme::smallSize - 1.0f, juce::Font::bold));
    const auto percent = [] (float load) { return juce::String (juce::roundToInt (load * 100.0f)) + "%"; };
    g.drawText (fresh ? percent (loadAverage) + " / " + percent (loadPeak) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")),
                inner, juce::Justification::centred, false);
}

// 位相相関: 上に -1〜+1 の横バー（中央が 0。0 から今の値まで塗る。入力の値を薄い縦線で）、下に数値
void PluginActivityView::paintCorrelation (juce::Graphics& g, juce::Rectangle<float> area)
{
    auto inner = area.reduced (7.0f, 5.0f);
    const auto track = inner.removeFromTop (inner.getHeight() * 0.42f).withSizeKeepingCentre (inner.getWidth(), 8.0f);

    g.setColour (Theme::card);
    g.fillRoundedRectangle (track, 3.0f);

    auto xAt = [&] (float value) { return track.getCentreX() + juce::jlimit (-1.0f, 1.0f, value) * track.getWidth() * 0.5f; };
    const auto colour = correlationOut < 0.0f ? Theme::error : (correlationOut < 0.3f ? Theme::warning : Theme::ok);

    if (fresh && correlationValid)
    {
        const float x = xAt (correlationOut);
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (x, track.getCentreX()), track.getY(),
                                                                juce::jmax (x, track.getCentreX()), track.getBottom()));
        g.setColour (Theme::subText.withAlpha (0.7f));
        g.fillRect (xAt (correlationIn) - 0.75f, track.getY() - 2.0f, 1.5f, track.getHeight() + 4.0f);
    }

    g.setColour (Theme::dimText);
    g.fillRect (track.getCentreX() - 0.5f, track.getY() - 3.0f, 1.0f, track.getHeight() + 6.0f);

    g.setColour (fresh && correlationValid ? colour : Theme::dimText);
    g.setFont (juce::FontOptions (Theme::smallSize - 1.0f, juce::Font::bold));
    g.drawText (fresh && correlationValid ? formatCorrelation (correlationOut) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")),
                inner, juce::Justification::centred, false);
}

// ゴニオメーター: 枠いっぱいに描く（数値は出さない。位相相関の数値は「位相相関」の表示で見る）。
// 縦が中央（L+R）、横が左右の差（L-R）。行は横長で低いので、下半分の点は原点を挟んだ反対側（上半分）に折り返し
// （音の波形は正負がほぼ対称なので形の見え方は変わらない）、下端の中央を原点にして横は枠の幅いっぱいに広げる。
// 目安の線: 縦の中央（モノラル）と、原点から左上・右上の角（L / R だけの音）
void PluginActivityView::paintGoniometer (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto inner = area.reduced (3.0f);
    const juce::Point<float> origin (inner.getCentreX(), inner.getBottom());
    const float xScale = inner.getWidth() * 0.5f, yScale = inner.getHeight();

    g.setColour (Theme::card);
    g.drawLine (origin.x, inner.getY(), origin.x, origin.y, 1.0f);
    g.drawLine (origin.x, origin.y, inner.getX(), inner.getY(), 1.0f);
    g.drawLine (origin.x, origin.y, inner.getRight(), inner.getY(), 1.0f);

    if (fresh)
    {
        g.setColour (Theme::accent.withAlpha (0.65f));
        for (auto point : goniometer)
        {
            if (point.y < 0.0f)
                point = -point;
            g.fillRect (origin.x - point.x * xScale - 0.7f, origin.y - point.y * yScale - 0.7f, 1.4f, 1.4f);
        }
    }
}

//==============================================================================
void PluginSnapshotView::update (const PluginChain::Plugin* plugin)
{
    const int version = plugin != nullptr ? plugin->snapshotVersion : -1;
    if (version == snapshotVersion)
        return;

    snapshotVersion = version;
    snapshot = plugin != nullptr ? plugin->snapshot : juce::Image();
    repaint();
}

// 撮った静止画を縦横比を保って収める。まだなければ案内
void PluginSnapshotView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (Theme::rackBackground);
    g.fillRoundedRectangle (bounds, 5.0f);

    if (snapshot.isValid())
    {
        const juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (bounds, 5.0f);
        g.reduceClipRegion (clip);
        const auto r = getLocalBounds().reduced (1);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImageWithin (snapshot, r.getX(), r.getY(), r.getWidth(), r.getHeight(), juce::RectanglePlacement::centred);
    }
    else
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (Theme::smallSize - 2.0f));
        g.drawFittedText (TRANS ("No snapshot"), getLocalBounds().reduced (3, 2), juce::Justification::centred, 2, 0.8f);
    }

    g.setColour (Theme::cardBorder);
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
}
