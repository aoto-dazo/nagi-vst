#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace NagiColours
{
    const juce::Colour bg        { 0xff0d1014 };
    const juce::Colour panel     { 0xff151a21 };
    const juce::Colour panelEdge { 0xff232a33 };
    const juce::Colour grid      { 0xff1f262f };
    const juce::Colour text      { 0xffd9dee5 };
    const juce::Colour textDim   { 0xff7d8794 };
    const juce::Colour accent    { 0xff5fd3c6 }; // 凪の水面
    const juce::Colour cut       { 0xffff7a59 }; // 削った量
}

class NagiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    NagiLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (NagiAudioProcessor&);
    ~SpectrumView() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    float xForFreq (float hz) const;
    float freqForX (float x) const;
    float yForDb (float db) const;
    juce::Rectangle<float> plotArea() const;

    NagiAudioProcessor& proc;
    static constexpr int numCols = 360;
    std::vector<float> inCol, cutCol, peakCut;
    int lastFrame = -1;
    float maxCutDisplay = 0.0f;

    enum class Handle { none, low, high };
    Handle dragging = Handle::none, hover = Handle::none;
    Handle handleAt (float x) const;
};

//==============================================================================
class NagiAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NagiAudioProcessorEditor (NagiAudioProcessor&);
    ~NagiAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void addKnob (Knob&, const juce::String& paramId, const juce::String& title, const juce::String& tooltip);

    NagiAudioProcessor& proc;
    NagiLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    SpectrumView spectrum;

    Knob depth, sharpness, selectivity, attack, release, low, high, tilt, mix, output;
    Knob* knobs[10] = { &depth, &sharpness, &selectivity, &attack, &release, &low, &high, &tilt, &mix, &output };

    juce::TextButton lowLatencyButton { "LOW LATENCY" }, highQualityButton { "HIGH QUALITY" }, deltaButton { "DELTA" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deltaAttachment;
    juce::Label latencyLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NagiAudioProcessorEditor)
};
