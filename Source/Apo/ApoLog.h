#pragma once

namespace pri::apo
{
// %ProgramData%\PRISystemRack\Logs\apo.log に 1 行追記する（OutputDebugString にも出す）。
// 1 MB を超えたら apo.old.log に回して新しく書き始める（ファイルは最大 2 つ。前の apo.old.log は消える）。
// ファイルを開いて閉じるので、APOProcess などリアルタイムの処理からは呼ばないこと。
void Log(const wchar_t* format, ...);
} // namespace pri::apo
