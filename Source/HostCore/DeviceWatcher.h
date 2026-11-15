#pragma once

#include <cstdint>
#include <memory>

namespace pri::host
{
// 再生デバイスの変化（追加・削除・有効/無効・プロパティの変更）を Windows から通知してもらい、回数を数える。
// 通知は Windows のスレッドから来るので、使う側は GetChangeCount() をときどき読んで、増えていたら一覧を読み直す。
// Equalizer APO の設定ツールなどがレジストリを直接書き換えた場合は通知が来ないことがある
class DeviceWatcher
{
public:
    DeviceWatcher();  // 作ったスレッドで COM を初期化する（壊すのも同じスレッドで）
    ~DeviceWatcher();

    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    // 通知を受け取れているか（登録に失敗したら false。そのときは回数は増えない）
    bool IsWatching() const;

    // 再生デバイスについての通知を受けた回数（どのスレッドから読んでもよい）
    uint32_t GetChangeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace pri::host
