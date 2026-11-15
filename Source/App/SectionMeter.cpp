#include "SectionMeter.h"

#include "Theme.h"

namespace
{
using Kind = SectionMeter::Kind;

float toDb (double value, float floor)
{
    return value > 0.0 ? juce::jmax (floor, (float) (20.0 * std::log10 (value))) : floor;
}

float powerToDb (double meanSquare, float floor)
{
    return meanSquare > 0.0 ? juce::jmax (floor, (float) (10.0 * std::log10 (meanSquare))) : floor;
}

float loudnessOf (double sum, double frames, float floor)
{
    return frames > 0.0 && sum > 0.0 ? juce::jmax (floor, (float) (-0.691 + 10.0 * std::log10 (sum / frames))) : floor;
}

constexpr float aes17 = 3.01f; // RMS をサイン波のピークにそろえる分（VU と K-System）

// 種類ごとの目盛り。値（バーの位置）は dBFS 相当（ラウドネスは LUFS）で、表示する数値は value * factor + offset
struct Scale
{
    float minValue = -60.0f, maxValue = 0.0f;  // バーの左端・右端
    std::vector<std::pair<float, juce::String>> ticks; // 位置（値）と目盛りの数値
    float warnFrom = 1000.0f, redFrom = 1000.0f;       // ここから黄色・赤
    bool zones = false;  // true: バーを位置で色分けする（K-System、VU）。false: 値に合わせてバー全体の色を変える（ピーク、RMS）
    float factor = 1.0f, offset = 0.0f;
    bool showSign = false; // 正の値に + を付ける
    int decimals = 1;
    juce::String unit;
    juce::Colour colour = Theme::accent;
};

Scale makeScale (Kind kind)
{
    Scale s;
    // 表示の数値で目盛りを並べる（位置は数値から逆算する）
    const auto addTicks = [&s] (std::initializer_list<float> displayValues)
    {
        for (const float d : displayValues)
        {
            const float position = (d - s.offset) / s.factor;
            if (position >= s.minValue - 0.01f && position <= s.maxValue + 0.01f)
                s.ticks.emplace_back (position, (s.showSign && d > 0.0f ? "+" : "") + juce::String (juce::roundToInt (d)));
        }
    };
    const auto kSystem = [&] (float zero)
    {
        s.minValue = -60.0f; s.maxValue = 0.0f;
        s.offset = zero; s.showSign = true; s.unit = "dB"; s.zones = true; s.colour = Theme::ok;
        s.warnFrom = -zero; s.redFrom = -zero + 4.0f;
        addTicks ({ -40.0f, -30.0f, -20.0f, -10.0f, 0.0f, 4.0f, 8.0f, zero });
    };

    switch (kind)
    {
        case Kind::peak:
        case Kind::rms:
        case Kind::peakRms:
            s.unit = "dB"; s.redFrom = s.warnFrom = -1.0f; s.decimals = kind == Kind::peak ? 0 : 1;
            addTicks ({ -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f, 0.0f });
            break;
        case Kind::vu:
            s.offset = 18.0f; s.minValue = -38.0f; s.maxValue = -15.0f; // -20〜+3 VU
            s.showSign = true; s.unit = "VU"; s.redFrom = s.warnFrom = -18.0f; s.zones = true;
            addTicks ({ -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f });
            break;
        case Kind::lufsMomentary:
        case Kind::lufsShortTerm:
        case Kind::lufsIntegrated:
            s.minValue = -60.0f; s.maxValue = 0.0f; s.unit = "LUFS";
            addTicks ({ -50.0f, -40.0f, -30.0f, -23.0f, -18.0f, -14.0f, -9.0f, 0.0f });
            break;
        case Kind::k20: kSystem (20.0f); break;
        case Kind::k14: kSystem (14.0f); break;
        case Kind::k12: kSystem (12.0f); break;
        case Kind::none: break;
    }
    return s;
}

const Scale& scaleFor (Kind kind)
{
    static const auto scales = []
    {
        std::vector<Scale> all;
        for (int i = 0; i <= (int) Kind::none; ++i)
            all.push_back (makeScale ((Kind) i));
        return all;
    }();
    return scales[(size_t) kind];
}

bool isLoudness (Kind kind) { return kind == Kind::lufsMomentary || kind == Kind::lufsShortTerm || kind == Kind::lufsIntegrated; }
bool hasPeakOverlay (Kind kind) { return kind == Kind::peakRms || kind == Kind::k20 || kind == Kind::k14 || kind == Kind::k12; }

float binLoudness (int bin) { return -70.0f + ((float) bin + 0.5f) * 0.1f; }

// インテグレーテッドのヒストグラムの区分ごとのエネルギー（10^((L + 0.691) / 10)。更新のたびに pow を計算しないよう前もって作る）
const std::vector<double>& binEnergies (int bins)
{
    static const auto energies = [bins]
    {
        std::vector<double> e ((size_t) bins);
        for (int bin = 0; bin < bins; ++bin)
            e[(size_t) bin] = std::pow (10.0, ((double) binLoudness (bin) + 0.691) / 10.0);
        return e;
    }();
    return energies;
}
} // namespace

//==============================================================================
const std::vector<SectionMeter::Kind>& SectionMeter::order()
{
    static const std::vector<Kind> kinds { Kind::peak, Kind::rms, Kind::peakRms, Kind::vu,
                                           Kind::lufsMomentary, Kind::lufsShortTerm, Kind::lufsIntegrated,
                                           Kind::k20, Kind::k14, Kind::k12, Kind::none };
    return kinds;
}

SectionMeter::Kind SectionMeter::next (Kind kind, int steps)
{
    const auto& kinds = order();
    const int count = (int) kinds.size();
    int index = 0;
    for (int i = 0; i < count; ++i)
        if (kinds[(size_t) i] == kind)
            index = i;
    return kinds[(size_t) (((index + steps) % count + count) % count)];
}

bool SectionMeter::startsGroup (Kind kind)
{
    return kind == Kind::lufsMomentary || kind == Kind::k20 || kind == Kind::none;
}

juce::String SectionMeter::name (Kind kind)
{
    switch (kind)
    {
        case Kind::peak:           return TRANS ("Peak");
        case Kind::rms:            return "RMS";
        case Kind::peakRms:        return TRANS ("Peak + RMS");
        case Kind::vu:             return "VU";
        case Kind::lufsMomentary:  return TRANS ("Loudness: momentary (LUFS)");
        case Kind::lufsShortTerm:  return TRANS ("Loudness: short-term (LUFS)");
        case Kind::lufsIntegrated: return TRANS ("Loudness: integrated (LUFS)");
        case Kind::k20:            return "K-System K-20";
        case Kind::k14:            return "K-System K-14";
        case Kind::k12:            return "K-System K-12";
        case Kind::none:           return TRANS ("None");
    }
    return {};
}

juce::String SectionMeter::toId (Kind kind)
{
    switch (kind)
    {
        case Kind::peak:           return "peak";
        case Kind::rms:            return "rms";
        case Kind::peakRms:        return "peakRms";
        case Kind::vu:             return "vu";
        case Kind::lufsMomentary:  return "lufsM";
        case Kind::lufsShortTerm:  return "lufsS";
        case Kind::lufsIntegrated: return "lufsI";
        case Kind::k20:            return "k20";
        case Kind::k14:            return "k14";
        case Kind::k12:            return "k12";
        case Kind::none:           return "none";
    }
    return "peak";
}

SectionMeter::Kind SectionMeter::fromId (const juce::String& id)
{
    for (const auto kind : order())
        if (toId (kind) == id)
            return kind;
    return Kind::peakRms;
}

//==============================================================================
void SectionMeter::update (const LevelReading& reading, Kind kind)
{
    const auto now = juce::Time::getMillisecondCounter();
    const float dt = lastUpdateMs == 0 ? 0.05f : juce::jlimit (0.0f, 0.5f, (float) (now - lastUpdateMs) / 1000.0f);
    lastUpdateMs = now;

    // ピーク: 新しい値がなければ一定の速さで落とす
    for (int side = 0; side < 2; ++side)
        peakDb[side] = juce::jmax (toDb (reading.peak[side], floorDb), peakDb[side] - 40.0f * dt);

    // 時間窓（RMS 300 ms、モーメンタリー 400 ms、ショートターム 3 秒）
    Entry entry;
    entry.timeMs = now;
    for (int side = 0; side < 2; ++side)
    {
        entry.sumSquares[side] = reading.sumSquares[side];
        entry.samples[side] = reading.samples[side];
    }
    entry.loudnessSum = reading.loudnessSum;
    entry.loudnessFrames = reading.loudnessFrames;
    history.push_back (entry);
    while (! history.empty() && now - history.front().timeMs > 3200)
        history.pop_front();

    Entry sum;
    windowSums (now, 300, sum);
    for (int side = 0; side < 2; ++side)
        rmsDb[side] = sum.samples[side] > 0.0 ? powerToDb (sum.sumSquares[side] / sum.samples[side], floorDb) : floorDb;

    windowSums (now, 400, sum);
    momentary = loudnessOf (sum.loudnessSum, sum.loudnessFrames, floorDb);
    windowSums (now, 3000, sum);
    shortTerm = loudnessOf (sum.loudnessSum, sum.loudnessFrames, floorDb);

    // インテグレーテッド: 100 ms ごとに直近 400 ms の値を数える（-70 LUFS 未満は数えない）。
    // タイマーが 100 ms より遅いときは、その間の回数だけ同じ値を数える
    if (lastGateBlockMs == 0)
        lastGateBlockMs = now;
    for (int steps = 0; now - lastGateBlockMs >= 100; ++steps)
    {
        if (steps >= 20) // 長く止まっていたとき（スリープなど）は数えずに進める
        {
            lastGateBlockMs = now;
            break;
        }
        lastGateBlockMs += 100;
        if (momentary > -70.0f)
            ++histogram[(size_t) juce::jlimit (0, histogramBins - 1, (int) ((momentary + 70.0f) * 10.0f))];
    }
    integrated = integratedLoudness();

    // ピークホールド: 表示しているバー（ピークを重ねる種類はピーク）の最大を 1.5 秒保ち、そのあと 20 dB/秒で下げる
    if (kind != holdKind)
    {
        holdKind = kind;
        holdDb[0] = holdDb[1] = floorDb;
    }
    for (int side = 0; side < 2; ++side)
    {
        const float value = holdSource (kind, side);
        if (value >= holdDb[side])
        {
            holdDb[side] = value;
            holdSinceMs[side] = now;
        }
        else if (now - holdSinceMs[side] > 1500)
        {
            holdDb[side] = juce::jmax (value, holdDb[side] - 20.0f * dt);
        }
    }
}

void SectionMeter::windowSums (juce::uint32 now, juce::uint32 windowMs, Entry& sum) const
{
    sum = {};
    for (auto it = history.rbegin(); it != history.rend(); ++it)
    {
        if (it != history.rbegin() && now - it->timeMs >= windowMs) // 最新の 1 回は必ず入れる
            break;
        for (int side = 0; side < 2; ++side)
        {
            sum.sumSquares[side] += it->sumSquares[side];
            sum.samples[side] += it->samples[side];
        }
        sum.loudnessSum += it->loudnessSum;
        sum.loudnessFrames += it->loudnessFrames;
    }
}

float SectionMeter::integratedLoudness() const
{
    // 1 回目のゲート（-70 LUFS。数える時点で済み）を通った区間の平均から 10 LU 下を 2 回目のゲートにして、その上の区間を平均する
    const auto& energies = binEnergies (histogramBins);
    const auto average = [this, &energies] (float gate)
    {
        double energy = 0.0, count = 0.0;
        for (int bin = 0; bin < histogramBins; ++bin)
        {
            if (histogram[(size_t) bin] == 0 || binLoudness (bin) < gate)
                continue;
            energy += (double) histogram[(size_t) bin] * energies[(size_t) bin];
            count += (double) histogram[(size_t) bin];
        }
        return count > 0.0 ? (float) (-0.691 + 10.0 * std::log10 (energy / count)) : floorDb;
    };

    const float ungated = average (-70.0f);
    return ungated <= floorDb ? floorDb : average (ungated - 10.0f);
}

void SectionMeter::resetIntegrated()
{
    histogram.fill (0);
    integrated = floorDb;
    if (holdKind == Kind::lufsIntegrated)
        holdDb[0] = holdDb[1] = floorDb;
}

float SectionMeter::mainValue (Kind kind, int side) const
{
    switch (kind)
    {
        case Kind::peak:           return peakDb[side];
        case Kind::rms:
        case Kind::peakRms:        return rmsDb[side];
        case Kind::vu:
        case Kind::k20:
        case Kind::k14:
        case Kind::k12:            return rmsDb[side] <= floorDb ? floorDb : rmsDb[side] + aes17;
        case Kind::none:           return floorDb;
        case Kind::lufsMomentary:  return momentary;
        case Kind::lufsShortTerm:  return shortTerm;
        case Kind::lufsIntegrated: return integrated;
    }
    return floorDb;
}

float SectionMeter::holdSource (Kind kind, int side) const
{
    return hasPeakOverlay (kind) ? peakDb[side] : mainValue (kind, side);
}

//==============================================================================
void SectionMeter::paint (juce::Graphics& g, Kind kind, juce::Rectangle<float> meter, juce::Rectangle<int> readout,
                          bool peakHold) const
{
    if (kind == Kind::none) // 表示しない: 枠も数値も描かない（場所は残す）
        return;

    const auto& scale = scaleFor (kind);
    const bool single = isLoudness (kind); // ラウドネスは左右を合わせた 1 本

    if (! meter.isEmpty())
    {
        const auto xAt = [&] (float value)
        {
            return meter.getX() + meter.getWidth() * juce::jlimit (0.0f, 1.0f, (value - scale.minValue) / (scale.maxValue - scale.minValue));
        };

        g.setColour (Theme::rackBackground);
        g.fillRoundedRectangle (meter, 4.0f);

        // 左（上）と右（下）の 2 本のバー（ラウドネスは枠いっぱいの 1 本）
        const auto inner = meter.reduced (1.5f);
        constexpr float barGap = 1.0f;
        const float barHeight = (inner.getHeight() - barGap) * 0.5f;
        const auto barArea = [&] (int side)
        {
            return single ? inner : inner.withHeight (barHeight).translated (0.0f, side == 0 ? 0.0f : barHeight + barGap);
        };
        const int sides = single ? 1 : 2;

        for (int side = 0; side < sides; ++side)
        {
            const auto bar = barArea (side);

            // ピークを薄く重ねる種類（ピーク＋RMS、K-System）は、先にピークを描く
            if (hasPeakOverlay (kind))
            {
                g.setColour (scale.colour.withAlpha (0.35f));
                g.fillRoundedRectangle (bar.withRight (juce::jmax (bar.getX(), xAt (peakDb[side]))), 2.0f);
            }

            const float value = mainValue (kind, side);
            const float right = juce::jmax (bar.getX(), xAt (value));
            if (scale.zones)
            {
                // 位置で色分け: warnFrom まで基本の色、redFrom まで黄色、その上は赤（K-System は 0 から黄色、+4 から赤）。
                // 色ごとに四角で塗るので、バー全体を角の丸い形で切り抜いてから塗る（色分けしない種類と同じ角の丸み）
                const float warnX = xAt (scale.warnFrom), redX = xAt (scale.redFrom);
                juce::Graphics::ScopedSaveState clipState (g);
                juce::Path rounded;
                rounded.addRoundedRectangle (bar.withRight (right), 2.0f);
                g.reduceClipRegion (rounded);
                g.setColour (scale.colour);
                g.fillRect (bar.withRight (juce::jmin (right, warnX)));
                if (right > warnX)
                {
                    g.setColour (Theme::warning);
                    g.fillRect (bar.withLeft (warnX).withRight (juce::jmin (right, redX)));
                }
                if (right > redX)
                {
                    g.setColour (juce::Colours::red);
                    g.fillRect (bar.withLeft (redX).withRight (right));
                }
            }
            else
            {
                g.setColour (value > scale.redFrom ? juce::Colours::red : value > scale.warnFrom ? Theme::warning : scale.colour);
                g.fillRoundedRectangle (bar.withRight (right), 2.0f);
            }
        }

        // 目盛り: バーの中の細い線と、下の数値。数値は右から並べ、間が詰まるものは省く
        g.setFont (juce::FontOptions (10.0f));
        float lastLabelLeft = 1.0e6f;
        for (auto it = scale.ticks.rbegin(); it != scale.ticks.rend(); ++it)
        {
            const float x = xAt (it->first);
            g.setColour (Theme::windowBackground.withAlpha (0.55f));
            g.fillRect (x - 0.5f, meter.getY() + 1.0f, 1.0f, meter.getHeight() - 2.0f);
            g.setColour (Theme::dimText);
            g.fillRect (x - 0.5f, meter.getBottom() + 1.0f, 1.0f, 3.0f);

            const auto& label = it->second;
            const float labelWidth = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label) + 2.0f;
            const float left = juce::jlimit (meter.getX(), meter.getRight() - labelWidth, x - labelWidth * 0.5f);
            if (left + labelWidth <= lastLabelLeft - 4.0f)
            {
                g.drawText (label, juce::Rectangle<float> (left, meter.getBottom() + 4.0f, labelWidth, 11.0f), juce::Justification::centred, false);
                lastLabelLeft = left;
            }
        }

        // ピークホールド（縦線。それぞれのバーに）
        if (peakHold)
        {
            for (int side = 0; side < sides; ++side)
            {
                if (holdDb[side] <= scale.minValue)
                    continue;
                const auto bar = barArea (side);
                g.setColour (holdDb[side] > scale.redFrom ? juce::Colours::red : Theme::text);
                g.fillRect (juce::jlimit (bar.getX(), bar.getRight() - 2.0f, xAt (holdDb[side]) - 1.0f), bar.getY(), 2.0f, bar.getHeight());
            }
        }

        g.setColour (Theme::cardBorder);
        g.drawRoundedRectangle (meter, 4.0f, 1.0f);
    }

    // 右の数値（左右の大きい方。表示の単位で）
    const float value = juce::jmax (mainValue (kind, 0), single ? floorDb : mainValue (kind, 1));
    juce::String text;
    if (value <= floorDb + 0.5f || (single && value <= -70.0f))
    {
        text = "-inf";
    }
    else
    {
        const float display = value * scale.factor + scale.offset;
        text = (scale.showSign && display > 0.0f ? "+" : "")
             + (scale.decimals > 0 ? juce::String (display, scale.decimals) : juce::String (juce::roundToInt (display)));
    }
    if (scale.unit.isNotEmpty())
        text << " " << scale.unit;

    g.setColour (Theme::subText);
    g.setFont (juce::FontOptions (Theme::smallSize));
    g.drawText (text, readout, juce::Justification::centredRight, false);
}
