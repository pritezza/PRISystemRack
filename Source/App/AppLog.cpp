#include "AppLog.h"

#include <process.h> // _getpid

namespace AppLog
{
namespace
{
constexpr juce::int64 maxLogBytes = 1024 * 1024;
juce::CriticalSection lock;

juce::File logFolder()
{
    return juce::File (juce::SystemStats::getEnvironmentVariable ("ProgramData", "C:\\ProgramData"))
        .getChildFile ("PRISystemRack")
        .getChildFile ("Logs");
}

// 書けた名前（"app"、"app-<ユーザー名>" など）。書けなくなるまで使い続ける
juce::String usedBaseName;

// 書く名前の候補（この順に試す）。ProgramData の下のファイルは作った Windows ユーザーしか書けない（ほかのユーザーは読むだけ）ので、
// ほかのユーザーが作った app.log には書けない。そのときは app-<ユーザー名>.log、それもほかの人に先に作られていれば
// app-<ユーザー名>-2.log に書く（ログフォルダーの権限は広げない。管理者として動く Setup も書くフォルダーなので、
// 一般ユーザーが中のファイルを置き換えられないようにしておく）
juce::StringArray candidateNames()
{
    const auto user = juce::File::createLegalFileName (juce::SystemStats::getLogonName());
    return { "app", "app-" + user, "app-" + user + "-2" };
}

// baseName.log に 1 行追記する（上限を超えていたら baseName.old.log に回す）。開けない、または回せない
// （.old.log がほかのユーザーのものなど。回せないまま書き続けると上限なく大きくなる）なら false
bool append (const juce::String& baseName, const juce::String& line)
{
    const auto log = logFolder().getChildFile (baseName + ".log");
    if (log.getSize() >= maxLogBytes && ! log.moveFileTo (log.getSiblingFile (baseName + ".old.log"))) // 前の .old.log は上書きされて消える
        return false;

    juce::FileOutputStream out (log); // 追記（なければ作る）
    if (! out.openedOk())
        return false;
    out.writeText (line, false, false, nullptr);
    out.flush();
    return out.getStatus().wasOk();
}
} // namespace

juce::File file()
{
    return logFolder().getChildFile ((usedBaseName.isNotEmpty() ? usedBaseName : juce::String ("app")) + ".log");
}

void write (const juce::String& message)
{
    const auto now = juce::Time::getCurrentTime();
    const auto line = now.formatted ("%Y-%m-%d %H:%M:%S") + juce::String::formatted (".%03d", now.getMilliseconds())
                      + " [pid " + juce::String (_getpid()) + "] "
                      + message.replace ("\r", " ").replace ("\n", " / ") + "\r\n";

    const juce::ScopedLock scope (lock);
    logFolder().createDirectory();

    // 前に書けた名前があれば、まずそれに書く。書けなくなっていたら（ほかのユーザーに置き換えられたなど）、候補を初めから試し直す
    if (usedBaseName.isNotEmpty() && append (usedBaseName, line))
        return;
    usedBaseName.clear();

    for (const auto& baseName : candidateNames())
        if (append (baseName, line))
        {
            usedBaseName = baseName;
            return;
        }
}
} // namespace AppLog
