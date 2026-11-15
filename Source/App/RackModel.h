#pragma once

#include <juce_events/juce_events.h>

#include "LevelMeterTap.h"
#include "PluginChain.h"

// ChainView（上から下へ並べる画面）が表示するラック。メインのラック（RackEngine）とアプリ用ラック（AppRack）が実装する
class RackModel
{
public:
    virtual ~RackModel() = default;

    virtual PluginChain& getChain() = 0;

    // 表示に関わる値（入力の説明、合流の一覧など）が変わったら知らせる
    virtual juce::ChangeBroadcaster& getModelBroadcaster() = 0;

    virtual float getInputGainDb() const = 0;
    virtual void setInputGainDb (float db) = 0;
    virtual LevelReading takeInputLevels() = 0; // 入力の欄のメーターの値（前回読んでからの分。読むと 0 に戻る）

    virtual float getOutputGainDb() const = 0;
    virtual void setOutputGainDb (float db) = 0;
    virtual LevelReading takeOutputLevels() = 0;

    virtual juce::String getInputSubtitle() const = 0;  // 入力・出力の欄の見出しの右の説明（空なら出さない）
    virtual juce::String getOutputSubtitle() const = 0;
    virtual bool outputIsMerge() const { return false; } // 最後の欄を「出力」ではなく「合流」と表示する

    // 最後の欄に出す、最終的なレイテンシー（プラグインの合計など）
    virtual juce::String describeTotalLatency() const = 0;

    // 出力の手前で合流するアプリ用ラック（メインのラックだけが返す。出力の欄の下に名前の札を出す。バイパス中は札の色を変える）
    struct MergedRack
    {
        juce::String name;
        bool bypassed = false;
        bool operator== (const MergedRack& other) const { return name == other.name && bypassed == other.bypassed; }
    };
    virtual std::vector<MergedRack> getMergedRacks() const { return {}; }
};
