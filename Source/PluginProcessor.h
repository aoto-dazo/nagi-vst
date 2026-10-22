#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "NagiEngine.h"

class NagiAudioProcessor : public juce::AudioProcessor
{
public:
    enum Mode { lowLatency = 0, highQuality = 1 };

    NagiAudioProcessor();
    ~NagiAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Nagi"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    NagiDisplayData display;

    int getActiveMode() const noexcept { return activeMode.load(); }
    double getLatencyMs() const noexcept
    {
        const double sr = getSampleRate() > 0 ? getSampleRate() : 48000.0;
        return 1000.0 * (double) getLatencySamples() / sr;
    }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static int lowLatencyOrder (double sr)  { return sr > 64000.0 ? 9 : 8; }   // 256 (48k) / 512 (96k)
    static int highQualityOrder (double sr) { return sr > 64000.0 ? 12 : 11; } // 2048 (48k) / 4096 (96k)

    NagiEngine engines[2];
    std::atomic<int> activeMode { highQuality };

    std::atomic<float>* pDepth = nullptr; std::atomic<float>* pSharp = nullptr; std::atomic<float>* pSel = nullptr;
    std::atomic<float>* pAtk = nullptr;   std::atomic<float>* pRel = nullptr;
    std::atomic<float>* pLow = nullptr;   std::atomic<float>* pHigh = nullptr; std::atomic<float>* pTilt = nullptr;
    std::atomic<float>* pMix = nullptr;   std::atomic<float>* pOut = nullptr;
    std::atomic<float>* pDelta = nullptr; std::atomic<float>* pMode = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NagiAudioProcessor)
};
