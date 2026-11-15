#pragma once

#include <juce_core/juce_core.h>

// プラグインチェーンのプリセット（メインのラックとアプリ用ラックの構成まるごと）。
// %APPDATA%\PRISystemRack\ChainPresets.xml に保存する（設定ファイルとは分ける。プリセットはプラグインの状態を
// まるごと含むので大きくなりやすく、設定ファイルは頻繁に書き直すため）。
//
//   <CHAINPRESETS version="1">
//     <PRESET id="{UUID}" name="名前" modified="ISO 8601">
//       <PRISystemRack .../>   ← RackEngine::saveState の中身
//     </PRESET>
//   </CHAINPRESETS>
//
// メッセージスレッドだけで使う
class ChainPresets
{
public:
    struct Preset
    {
        juce::String id;
        juce::String name;
    };

    ChainPresets(); // ファイルから読む

    juce::Array<Preset> getPresets() const;
    juce::String getName (const juce::String& id) const;
    bool contains (const juce::String& id) const { return find (id) != nullptr; }

    // 保存してある構成（RackEngine::saveState の XML）。なければ nullptr
    std::unique_ptr<juce::XmlElement> getState (const juce::String& id) const;

    juce::String add (const juce::String& name, const juce::XmlElement& state); // 追加して ID を返す
    bool overwrite (const juce::String& id, const juce::XmlElement& state);
    juce::String duplicate (const juce::String& id, const juce::String& name); // 元のすぐ下に複製して ID を返す（なければ空）
    bool rename (const juce::String& id, const juce::String& newName);
    bool remove (const juce::String& id);
    bool move (const juce::String& id, int newIndex); // 並び順を変える（newIndex は移動後の位置）

    // 同じ名前があれば、後ろに番号を付けて重ならない名前にする
    juce::String makeUniqueName (const juce::String& name, const juce::String& ignoreId = {}) const;

    static juce::File getFile();

private:
    juce::XmlElement* find (const juce::String& id) const;
    void save();

    std::unique_ptr<juce::XmlElement> root;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainPresets)
};
