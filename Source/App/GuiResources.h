#pragma once

// このプロセスが使っている Windows の描画資源の数。GDI オブジェクトは 1 つのプロセスで既定 10,000 個まで
// （使い切ると窓やカーソルを作れなくなり、描けない・落ちる）
namespace GuiResources
{
int gdiObjects();
int userObjects();
constexpr int gdiLimit = 10000;
} // namespace GuiResources
