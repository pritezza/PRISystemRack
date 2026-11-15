// PRICrashTest: プラグインが落ちたときの守り（PluginGuard）を確かめるためのテスト用 VST3。
// 音はそのまま通し、「落ちるまでの秒数」だけ処理したら processBlock の中でアクセス違反を起こす。
// 0 秒にすると落ちない。

#include <juce_audio_processors/juce_audio_processors.h>

class CrashTestProcessor final : public juce::AudioProcessor
{
public:
    CrashTestProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (crashAfterSeconds = new juce::AudioParameterFloat (juce::ParameterID { "crashAfter", 1 },
                                                                         "Crash after (s)", 0.0f, 60.0f, 3.0f));
    }

    const juce::String getName() const override { return "PRICrashTest"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void prepareToPlay (double sampleRate, int) override
    {
        currentSampleRate = sampleRate;
        processedSamples = 0;
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == layouts.getMainOutputChannelSet()
            && ! layouts.getMainInputChannelSet().isDisabled();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        processedSamples += buffer.getNumSamples();

        const float limit = crashAfterSeconds->get();
        if (limit > 0.0f && (double) processedSamples > (double) limit * currentSampleRate)
        {
            volatile int* nothing = nullptr;
            *nothing = 1; // わざとアクセス違反を起こす
        }
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        juce::MemoryOutputStream (destData, true).writeFloat (crashAfterSeconds->get());
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (sizeInBytes >= (int) sizeof (float))
            *crashAfterSeconds = juce::MemoryInputStream (data, (size_t) sizeInBytes, false).readFloat();
    }

private:
    juce::AudioParameterFloat* crashAfterSeconds = nullptr;
    double currentSampleRate = 48000.0;
    juce::int64 processedSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrashTestProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CrashTestProcessor();
}
