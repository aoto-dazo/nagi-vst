#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::NormalisableRange<float> freqRange()
    {
        juce::NormalisableRange<float> r (20.0f, 20000.0f);
        r.setSkewForCentre (630.0f);
        return r;
    }

    juce::String hzText (float v, int)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                            : juce::String ((int) std::round (v)) + " Hz";
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout NagiAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto dbAttr  = AudioParameterFloatAttributes().withLabel ("dB");
    auto msAttr  = AudioParameterFloatAttributes().withLabel ("ms");
    auto hzAttr  = AudioParameterFloatAttributes().withStringFromValueFunction (hzText);

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "depth", 1 }, "Depth",
                     NormalisableRange<float> (0.0f, 20.0f, 0.01f), 6.0f, dbAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sharpness", 1 }, "Sharpness",
                     NormalisableRange<float> (0.0f, 10.0f, 0.01f), 4.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "selectivity", 1 }, "Selectivity",
                     NormalisableRange<float> (0.0f, 10.0f, 0.01f), 4.0f));

    NormalisableRange<float> atk (0.5f, 50.0f, 0.01f); atk.setSkewForCentre (5.0f);
    NormalisableRange<float> rel (5.0f, 500.0f, 0.1f); rel.setSkewForCentre (80.0f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "attack", 1 },  "Attack",  atk, 5.0f,  msAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release", rel, 80.0f, msAttr));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "low", 1 },  "Low",  freqRange(), 300.0f,   hzAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "high", 1 }, "High", freqRange(), 20000.0f, hzAttr));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tilt", 1 }, "Tilt",
                     NormalisableRange<float> (-3.0f, 3.0f, 0.01f), 0.0f,
                     AudioParameterFloatAttributes().withLabel ("dB/oct")));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
                     NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                     AudioParameterFloatAttributes().withLabel ("%")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
                     NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, dbAttr));

    p.push_back (std::make_unique<AudioParameterBool>   (ParameterID { "delta", 1 }, "Delta", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                     StringArray { "Low Latency", "High Quality" }, 1));

    return { p.begin(), p.end() };
}

NagiAudioProcessor::NagiAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "NagiState", createLayout())
{
    pDepth = apvts.getRawParameterValue ("depth");
    pSharp = apvts.getRawParameterValue ("sharpness");
    pSel   = apvts.getRawParameterValue ("selectivity");
    pAtk   = apvts.getRawParameterValue ("attack");
    pRel   = apvts.getRawParameterValue ("release");
    pLow   = apvts.getRawParameterValue ("low");
    pHigh  = apvts.getRawParameterValue ("high");
    pTilt  = apvts.getRawParameterValue ("tilt");
    pMix   = apvts.getRawParameterValue ("mix");
    pOut   = apvts.getRawParameterValue ("output");
    pDelta = apvts.getRawParameterValue ("delta");
    pMode  = apvts.getRawParameterValue ("mode");

    for (auto& e : engines)
        e.setDisplay (&display);

    // 初期レイテンシー（prepare前に聞かれても妥当な値を返す）
    engines[lowLatency].prepare (48000.0, lowLatencyOrder (48000.0));
    engines[highQuality].prepare (48000.0, highQualityOrder (48000.0));
    activeMode = (int) pMode->load();
    setLatencySamples (engines[activeMode.load()].getLatency());
}

bool NagiAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void NagiAudioProcessor::prepareToPlay (double sampleRate, int)
{
    // 両モードとも確保しておき、切り替え時にメモリ確保が起きないようにする
    engines[lowLatency].prepare (sampleRate, lowLatencyOrder (sampleRate));
    engines[highQuality].prepare (sampleRate, highQualityOrder (sampleRate));

    activeMode = juce::jlimit (0, 1, (int) pMode->load());
    setLatencySamples (engines[activeMode.load()].getLatency());
}

void NagiAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int c = numIn; c < numOut; ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    const int mode = juce::jlimit (0, 1, (int) pMode->load());
    if (mode != activeMode.load())
    {
        activeMode = mode;
        engines[mode].reset();
        setLatencySamples (engines[mode].getLatency());
    }

    NagiSettings s;
    s.depthDb     = pDepth->load();
    s.sharpness   = pSharp->load();
    s.selectivity = pSel->load();
    s.attackMs    = pAtk->load();
    s.releaseMs   = pRel->load();
    s.lowHz       = pLow->load();
    s.highHz      = pHigh->load();
    s.tiltDbOct   = pTilt->load();
    s.mix         = pMix->load() * 0.01f;
    s.outGainDb   = pOut->load();
    s.delta       = pDelta->load() > 0.5f;

    const int ch = std::min (numIn, NagiEngine::maxChannels);
    if (ch > 0)
        engines[mode].process (buffer.getArrayOfWritePointers(), ch, buffer.getNumSamples(), s);
}

juce::AudioProcessorEditor* NagiAudioProcessor::createEditor()
{
    return new NagiAudioProcessorEditor (*this);
}

void NagiAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void NagiAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NagiAudioProcessor();
}
