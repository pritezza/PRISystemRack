#include "PluginLoadDialog.h"

#include "DialogLaunch.h"
#include "PluginSafety.h"
#include "ScaledContent.h"
#include "Text.h"
#include "Theme.h"

#include <iterator>

namespace
{
using Done = std::function<void (bool loaded, bool cancelled, const juce::String& error)>;
using Load = std::function<bool (juce::String& error)>;

const char* const stages[] = { "binary", "load", "create", "prepare", "process", "state" };

// ダイアログの中身
class LoadView final : public juce::Component,
                       private juce::Timer
{
public:
    LoadView (const juce::PluginDescription& descriptionToLoad, bool runProbe, Load loadFunction, Done doneCallback)
        : description (descriptionToLoad), load (std::move (loadFunction)), done (std::move (doneCallback))
    {
        cancelButton.setButtonText (TRANS ("Cancel"));
        Theme::styleButton (cancelButton);
        cancelButton.onClick = [this] { finish (false, true, {}); };
        addAndMakeVisible (cancelButton);

        // 試せない（補助プログラムがない）ときは、今までどおり読み込んでよいとする
        probing = runProbe && probe.start (description);
        setSize (480, probing ? 330 : 170);

        if (probing)
            startTimer (200);
        else
            beginLoading();
    }

    ~LoadView() override
    {
        // ダイアログが消えたので結果を知らせる（知らせた先でメッセージやプラグインの画面を出すので、ダイアログより後に出るように）。
        // 結果を決める前に消えた（ウィンドウの × や Esc で閉じられた）ときは、取りやめとして知らせる
        probe.cancel();
        report (false, true, {});
        juce::MessageManager::callAsync ([cb = done, loaded = resultLoaded, cancelled = resultCancelled, error = resultError]
                                         { if (cb != nullptr) cb (loaded, cancelled, error); });
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::windowBackground);
        auto area = getLocalBounds().reduced (22, 18);
        area.removeFromBottom (Theme::controlHeight + 12);

        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (Theme::titleSize, juce::Font::bold));
        g.drawText (loading ? TRANS ("Loading the plugin") : TRANS ("Checking the plugin"), area.removeFromTop (24),
                    juce::Justification::centredLeft, true);
        g.setColour (Theme::subText);
        g.setFont (juce::FontOptions (Theme::bodySize));
        g.drawText (description.name, area.removeFromTop (22), juce::Justification::centredLeft, true);
        area.removeFromTop (10);

        if (probing || probed)
        {
            // 段階の一覧（済んだもの ✓、今のもの ▶、失敗したもの ✕、まだのもの ·）
            const int current = stageIndex (progress.stage);
            for (int i = 0; i < (int) std::size (stages); ++i)
            {
                auto row = area.removeFromTop (21);
                const bool failed = failedStage >= 0 && i == failedStage;
                const bool doneStage = probed && ! failed ? (failedStage < 0 || i < failedStage) : i < current;
                const bool now = ! probed && i == current;

                juce::String mark = utf8 ("\xc2\xb7");                         // ·
                juce::Colour colour = Theme::dimText;
                if (failed)        { mark = utf8 ("\xe2\x9c\x95"); colour = Theme::error; }  // ✕
                else if (doneStage) { mark = utf8 ("\xe2\x9c\x93"); colour = Theme::ok; }     // ✓
                else if (now)       { mark = utf8 ("\xe2\x96\xb6"); colour = Theme::accent; } // ▶

                g.setColour (colour);
                g.setFont (juce::FontOptions (Theme::bodySize, juce::Font::bold));
                g.drawText (mark, row.removeFromLeft (22), juce::Justification::centred, false);
                g.setColour (now || failed ? Theme::text : (doneStage ? Theme::subText : Theme::dimText));
                g.setFont (juce::FontOptions (Theme::bodySize, now ? juce::Font::bold : juce::Font::plain));
                g.drawText (PluginSafety::stageName (stages[i]), row, juce::Justification::centredLeft, true);
                if (now)
                {
                    g.setColour (Theme::subText);
                    g.drawText (TRANS ("%seconds% s").replace ("%seconds%", juce::String ((int) progress.seconds)), row,
                                juce::Justification::centredRight, false);
                }
            }

            // 描画資源（使いすぎたら、その時点で試すのをやめて読み込まない）
            area.removeFromTop (8);
            g.setColour (Theme::subText);
            g.setFont (juce::FontOptions (Theme::smallSize));
            const auto count = [] (int value) { return value >= 0 ? juce::String (value) : juce::String ("-"); };
            g.drawText (TRANS ("GDI objects: %gdi%   USER objects: %user%   (not loaded if either reaches %limit%)")
                            .replace ("%gdi%", count (progress.gdi))
                            .replace ("%user%", count (progress.user))
                            .replace ("%limit%", juce::String (PluginSafety::gdiThreshold)),
                        area.removeFromTop (18), juce::Justification::centredLeft, true);
        }

        area.removeFromTop (8);
        g.setFont (juce::FontOptions (Theme::smallSize));
        if (loading)
        {
            g.setColour (Theme::warning);
            g.drawFittedText (TRANS ("Loading it into PRI System Rack. This cannot be cancelled, and the window does not respond until it finishes."),
                              area, juce::Justification::topLeft, 3);
        }
        else
        {
            g.setColour (Theme::dimText);
            g.drawFittedText (TRANS ("The plugin is tried in a separate process first, so that a plugin that crashes or uses up Windows resources "
                                     "does not take PRI System Rack down with it."),
                              area, juce::Justification::topLeft, 3);
        }
    }

    // 閉じてよいか（本体に読み込んでいる最中は × や Esc で閉じさせない。閉じると読み込みの途中で消えてしまうため）
    bool canClose() const { return ! loading; }

    void resized() override
    {
        auto buttons = getLocalBounds().reduced (22, 18).removeFromBottom (Theme::controlHeight);
        cancelButton.setBounds (buttons.removeFromRight (juce::jmax (90, Theme::buttonWidth (cancelButton))));
    }

private:
    static int stageIndex (const juce::String& stage)
    {
        for (int i = 0; i < (int) std::size (stages); ++i)
            if (stage == stages[i])
                return i;
        return 0; // まだ始まっていない（補助プログラムを起動しているところ）は最初の段階
    }

    void timerCallback() override
    {
        if (loading)
        {
            // 「読み込んでいます」を描いてから読み込む（読み込みの間は画面が止まるので、先に描かせておく）。
            // 読み込みの間はダイアログを閉じさせない（canClose）が、プラグインが中でメッセージを回すこともあるので、念のため
            // 読み込む処理は手元に写してから呼び、戻ったらこのダイアログがまだあるかを確かめる
            stopTimer();
            const auto loadNow = load;
            const juce::Component::SafePointer<LoadView> safe (this);
            juce::String error;
            const bool loaded = loadNow != nullptr && loadNow (error);
            if (safe == nullptr)
                return; // 読み込みの途中で消された（結果は消えるときに取りやめとして知らせてある）
            finish (loaded, false, error);
            return;
        }

        PluginSafety::Verdict verdict;
        const bool finished = probe.poll (progress, verdict);
        repaint();
        if (! finished)
            return;

        probing = false;
        probed = true;
        if (verdict.blocked)
        {
            // 失敗した段階に ✕ を付けて少し見せてから閉じる（すぐ閉じると、どこで止まったかが見えない）
            failedStage = stageIndex (verdict.stage);
            repaint();
            stopTimer();
            // 結果はここで決めておく（見せている間に × / Esc で閉じられても、理由が伝わるように）
            report (false, false, PluginSafety::describe (verdict, description.name));
            juce::Timer::callAfterDelay (800, [safe = juce::Component::SafePointer<LoadView> (this)]
            {
                if (safe != nullptr)
                    safe->closeWindow();
            });
            return;
        }

        beginLoading();
    }

    void beginLoading()
    {
        loading = true;
        cancelButton.setEnabled (false);
        repaint();
        startTimer (150); // 描かれるのを待ってから読み込む（timerCallback）
    }

    // 結果を決めて、ダイアログを閉じる（知らせるのは、ダイアログが消えたとき（デストラクター））。
    // 結果がもう決まっていれば（✕ を見せている間に［やめる］を押したなど）、先に決まった結果のまま閉じる
    void finish (bool loaded, bool cancelled, const juce::String& error)
    {
        report (loaded, cancelled, error);
        closeWindow();
    }

    void closeWindow()
    {
        stopTimer();
        probe.cancel();
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0); // ウィンドウと一緒にこの中身も消える（2 回目は何もしない）
    }

    // 結果を控える（最初の 1 回だけ）
    void report (bool loaded, bool cancelled, const juce::String& error)
    {
        if (reported)
            return;
        reported = true;
        resultLoaded = loaded;
        resultCancelled = cancelled;
        resultError = error;
    }

    const juce::PluginDescription description;
    Load load;
    Done done;
    PluginSafety::Probe probe;
    PluginSafety::Probe::Progress progress;
    bool probing = false, probed = false, loading = false, reported = false;
    bool resultLoaded = false, resultCancelled = true;
    juce::String resultError;
    int failedStage = -1;
    juce::TextButton cancelButton;
};

void launch (const juce::PluginDescription& description, bool runProbe, Load load, Done done)
{
    auto view = std::make_unique<LoadView> (description, runProbe, std::move (load), std::move (done));
    const juce::Component::SafePointer<LoadView> safeView (view.get());

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ScaledContent (std::move (view), Theme::effectiveScale()));
    options.dialogTitle = runProbe ? TRANS ("Checking the plugin") : TRANS ("Loading the plugin");
    options.dialogBackgroundColour = Theme::windowBackground;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;

    // 本体に読み込んでいる最中は、× / Esc / Alt+F4 で閉じさせない
    if (auto* window = dynamic_cast<DialogLaunch::Window*> (DialogLaunch::launchAsync (options)))
        window->canClose = [safeView] { return safeView == nullptr || safeView->canClose(); };
}
} // namespace

namespace PluginLoadDialog
{
void run (const juce::PluginDescription& description, Load load, Done done)
{
    if (! PluginSafety::isEnabled())
    {
        launch (description, false, std::move (load), std::move (done));
        return;
    }

    const auto* verdict = PluginSafety::findVerdict (description);
    if (verdict == nullptr || ! verdict->blocked)
    {
        // まだ試していなければ試す（試して問題なかったものは、起動している間は試さずに読み込む）
        launch (description, verdict == nullptr, std::move (load), std::move (done));
        return;
    }

    // 起動している間に試して、読み込めないと分かっている: 理由を出して、もう一度試すかを聞く
    // （時間切れや落ちたのは、PC が重かったなどでたまたまのこともある）
    const auto message = PluginSafety::describe (*verdict, description.name) + "\n\n" + TRANS ("Check it again?");
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, TRANS ("This plugin cannot be loaded"), message,
                                        TRANS ("Check again"), TRANS ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([description, load, done] (int result)
                                        {
                                            if (result == 0)
                                            {
                                                // 「チェックし直さない」は取りやめではない（まとめて足している途中なら、次のファイルへ進む）。
                                                // 理由はいま出したので、error は空にして重ねて出さない
                                                if (done != nullptr)
                                                    done (false, false, {});
                                                return;
                                            }
                                            PluginSafety::forget (description);
                                            launch (description, true, load, done);
                                        }));
}
} // namespace PluginLoadDialog
