#pragma once

#include <string>

namespace pri::host
{
// PRISystemRack APO が COM に登録されているか（インストール済みか）
bool IsApoRegistered();

// 本体と同じフォルダにある PRISystemRackSetup.exe のパス。なければ空
std::wstring FindSetupTool();

// RunSetupElevated で UAC の確認画面が出るか。出ないのは、すでに管理者として動いているとき、UAC が切られているとき、
// 管理者のアカウントで UAC が「通知しない」（ConsentPromptBehaviorAdmin = 0）のとき（どれも確認なしで実行される）
bool WillShowUacPrompt();

// PRISystemRackSetup.exe を管理者権限（UAC）で実行し、終わるまで待つ。
// 戻り値: 終了コード。UAC で拒否されたら -1、起動できなければ -2
int RunSetupElevated(const std::wstring& arguments);
} // namespace pri::host
