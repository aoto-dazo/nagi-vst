#include "PluginEditor.h"

using namespace NagiColours;

namespace
{
    constexpr float minHz = 20.0f, maxHz = 20000.0f;
    constexpr float topDb = 0.0f, bottomDb = -96.0f;
    constexpr float cutScaleDb = 24.0f; // 上部の削減量カーブのフルスケール

    juce::Font uiFont (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }
}

//==============================================================================
NagiLookAndFeel::NagiLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.3f));
    setColour (juce::Label::textColourId, text);
    setColour (juce::TooltipWindow::backgroundColourId, panel.brighter (0.1f));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, panelEdge);
    setColour (juce::TextEditor::backgroundColourId, panel);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.3f));
    setColour (juce::CaretComponent::caretColourId, accent);
}

void NagiLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float track = 3.0f;

    // 背景トラック
    juce::Path bgArc;
    bgArc.addCentredArc (centre.x, centre.y, radius - track, radius - track, 0.0f, startAngle, endAngle, true);
    g.setColour (panelEdge);
    g.strokePath (bgArc, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // 値（両極パラメータは中央から）
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    juce::Path valArc;
    valArc.addCentredArc (centre.x, centre.y, radius - track, radius - track, 0.0f,
                          std::min (from, angle), std::max (from, angle), true);
    const auto col = s.getName() == "depth" ? cut : accent;
    g.setColour (s.isEnabled() ? col : textDim);
    g.strokePath (valArc, juce::PathStrokeType (track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // ノブ本体
    const float knobR = radius - track * 3.0f;
    g.setGradientFill (juce::ColourGradient (panel.brighter (0.25f), centre.x, centre.y - knobR,
                                             panel.darker (0.3f), centre.x, centre.y + knobR, false));
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);
    g.setColour (panelEdge.brighter (0.2f));
    g.drawEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);

    // 指針
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -knobR + 3.0f, 3.0f, knobR * 0.45f, 1.5f);
    g.setColour (text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void NagiLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    const auto onCol = b.getName() == "delta" ? cut : accent;
    g.setColour (on ? onCol.withAlpha (0.18f) : panel.brighter (over ? 0.08f : 0.0f).darker (down ? 0.2f : 0.0f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (on ? onCol : panelEdge.brighter (0.15f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void NagiLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto onCol = b.getName() == "delta" ? cut : accent;
    g.setColour (b.getToggleState() ? onCol : textDim);
    g.setFont (uiFont (11.5f, true));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
}

juce::Label* NagiLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (uiFont (12.0f));
    l->setJustificationType (juce::Justification::centred);
    return l;
}

//==============================================================================
SpectrumView::SpectrumView (NagiAudioProcessor& p) : proc (p)
{
    inCol.assign (numCols, bottomDb);
    cutCol.assign (numCols, 0.0f);
    peakCut.assign (numCols, 0.0f);
    startTimerHz (30);
}

juce::Rectangle<float> SpectrumView::plotArea() const
{
    return getLocalBounds().toFloat().reduced (1.0f).withTrimmedLeft (34.0f).withTrimmedBottom (18.0f);
}

float SpectrumView::xForFreq (float hz) const
{
    const auto a = plotArea();
    const float t = std::log (hz / minHz) / std::log (maxHz / minHz);
    return a.getX() + t * a.getWidth();
}

float SpectrumView::freqForX (float x) const
{
    const auto a = plotArea();
    const float t = juce::jlimit (0.0f, 1.0f, (x - a.getX()) / a.getWidth());
    return minHz * std::pow (maxHz / minHz, t);
}

float SpectrumView::yForDb (float db) const
{
    const auto a = plotArea();
    return juce::jmap (juce::jlimit (bottomDb, topDb, db), topDb, bottomDb, a.getY(), a.getBottom());
}

void SpectrumView::resized() {}

void SpectrumView::timerCallback()
{
    auto& d = proc.display;
    const int frame = d.frameCounter.load (std::memory_order_acquire);
    const bool fresh = frame != lastFrame;
    lastFrame = frame;

    const int nb = d.numBins.load (std::memory_order_relaxed);
    const double sr = d.sampleRate.load (std::memory_order_relaxed);
    if (nb < 4) return;

    const double binHz = sr / (double) ((nb - 1) * 2);
    float frameMaxCut = 0.0f;

    for (int i = 0; i < numCols; ++i)
    {
        const float f0 = minHz * std::pow (maxHz / minHz, (float) i / numCols);
        const float f1 = minHz * std::pow (maxHz / minHz, (float) (i + 1) / numCols);
        const double b0 = f0 / binHz, b1 = f1 / binHz;

        float in = bottomDb, c = 0.0f;
        if (fresh)
        {
            if (b1 - b0 < 1.0)
            {
                // ビンが粗い帯域は線形補間
                const double bc = 0.5 * (b0 + b1);
                const int k = juce::jlimit (0, nb - 2, (int) bc);
                const float t = (float) (bc - k);
                in = juce::jmap (t, d.inputDb[k].load(), d.inputDb[k + 1].load());
                c  = juce::jmap (t, d.reductionDb[k].load(), d.reductionDb[k + 1].load());
            }
            else
            {
                for (int k = juce::jlimit (0, nb - 1, (int) b0); k <= juce::jlimit (0, nb - 1, (int) b1); ++k)
                {
                    in = std::max (in, d.inputDb[k].load());
                    c  = std::max (c,  d.reductionDb[k].load());
                }
            }
        }

        // 表示用のなめらかさ（上がるのは速く、下がるのはゆっくり）
        inCol[(size_t) i]  = in > inCol[(size_t) i] ? 0.6f * in + 0.4f * inCol[(size_t) i]
                                                    : std::max (in, inCol[(size_t) i] - 1.8f);
        cutCol[(size_t) i] = c > cutCol[(size_t) i] ? 0.7f * c + 0.3f * cutCol[(size_t) i]
                                                    : std::max (c, cutCol[(size_t) i] - 0.6f);
        frameMaxCut = std::max (frameMaxCut, cutCol[(size_t) i]);
    }

    maxCutDisplay = frameMaxCut > maxCutDisplay ? frameMaxCut : maxCutDisplay * 0.9f + frameMaxCut * 0.1f;
    repaint();
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto a = plotArea();

    g.setColour (panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    // グリッド
    g.setFont (uiFont (10.0f));
    const float freqs[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    const char* flabels[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };
    for (int i = 0; i < 8; ++i)
    {
        const float x = xForFreq (freqs[i]);
        g.setColour (grid);
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (textDim);
        g.drawText (flabels[i], juce::Rectangle<float> (x - 20, a.getBottom() + 2, 40, 14), juce::Justification::centred);
    }
    for (float db = -12.0f; db >= bottomDb + 1; db -= 12.0f)
    {
        const float y = yForDb (db);
        g.setColour (grid);
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (textDim);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (bounds.getX() + 2, y - 7, 30, 14), juce::Justification::centredRight);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // パス生成
    const float colW = a.getWidth() / (float) numCols;
    juce::Path inLine, outLine, inFill, cutBand, topCut;
    for (int i = 0; i < numCols; ++i)
    {
        const float x = a.getX() + ((float) i + 0.5f) * colW;
        const float yi = yForDb (inCol[(size_t) i]);
        const float yo = yForDb (inCol[(size_t) i] - cutCol[(size_t) i]);
        const float yc = a.getY() + juce::jlimit (0.0f, 1.0f, cutCol[(size_t) i] / cutScaleDb) * a.getHeight() * 0.42f;
        if (i == 0) { inLine.startNewSubPath (x, yi); outLine.startNewSubPath (x, yo); topCut.startNewSubPath (x, a.getY()); topCut.lineTo (x, yc); }
        else        { inLine.lineTo (x, yi); outLine.lineTo (x, yo); topCut.lineTo (x, yc); }
    }
    topCut.lineTo (a.getRight(), a.getY());
    topCut.closeSubPath();

    inFill = outLine;
    inFill.lineTo (a.getRight(), a.getBottom());
    inFill.lineTo (a.getX(), a.getBottom());
    inFill.closeSubPath();

    // 入力と出力の間（＝削った部分）を塗る
    cutBand = inLine;
    for (int i = numCols - 1; i >= 0; --i)
    {
        const float x = a.getX() + ((float) i + 0.5f) * colW;
        cutBand.lineTo (x, yForDb (inCol[(size_t) i] - cutCol[(size_t) i]));
    }
    cutBand.closeSubPath();

    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.22f), 0, a.getY(), accent.withAlpha (0.02f), 0, a.getBottom(), false));
    g.fillPath (inFill);

    g.setColour (cut.withAlpha (0.55f));
    g.fillPath (cutBand);

    g.setColour (textDim.withAlpha (0.6f));
    g.strokePath (inLine, juce::PathStrokeType (1.0f));
    g.setColour (accent);
    g.strokePath (outLine, juce::PathStrokeType (1.5f));

    // 上部: 削減量カーブ（下向きに伸びる）
    g.setColour (cut.withAlpha (0.25f));
    g.fillPath (topCut);
    g.setColour (cut);
    {
        juce::Path edge;
        for (int i = 0; i < numCols; ++i)
        {
            const float x = a.getX() + ((float) i + 0.5f) * colW;
            const float yc = a.getY() + juce::jlimit (0.0f, 1.0f, cutCol[(size_t) i] / cutScaleDb) * a.getHeight() * 0.42f;
            if (i == 0) edge.startNewSubPath (x, yc); else edge.lineTo (x, yc);
        }
        g.strokePath (edge, juce::PathStrokeType (1.3f));
    }

    // 処理範囲の外を暗く
    const float lowHz  = proc.apvts.getRawParameterValue ("low")->load();
    const float highHz = proc.apvts.getRawParameterValue ("high")->load();
    const float xl = xForFreq (lowHz), xh = xForFreq (highHz);
    g.setColour (bg.withAlpha (0.55f));
    g.fillRect (juce::Rectangle<float> (a.getX(), a.getY(), std::max (0.0f, xl - a.getX()), a.getHeight()));
    g.fillRect (juce::Rectangle<float> (xh, a.getY(), std::max (0.0f, a.getRight() - xh), a.getHeight()));

    for (auto [x, h] : { std::pair<float, Handle> { xl, Handle::low }, std::pair<float, Handle> { xh, Handle::high } })
    {
        const bool hot = hover == h || dragging == h;
        g.setColour (accent.withAlpha (hot ? 0.95f : 0.45f));
        g.drawLine (x, a.getY(), x, a.getBottom(), hot ? 2.0f : 1.0f);
        g.fillRoundedRectangle (x - 4.0f, a.getBottom() - 22.0f, 8.0f, 16.0f, 3.0f);
    }

    g.restoreState();

    // 表示ラベル
    g.setFont (uiFont (11.0f, true));
    g.setColour (cut);
    g.drawText ("MAX CUT  -" + juce::String (maxCutDisplay, 1) + " dB",
                juce::Rectangle<float> (a.getRight() - 160, a.getY() + 6, 150, 16), juce::Justification::centredRight);
    g.setColour (textDim);
    g.setFont (uiFont (10.0f));
    g.drawText ("drag the handles to set the range", juce::Rectangle<float> (a.getX() + 8, a.getY() + 6, 260, 14),
                juce::Justification::centredLeft);
}

SpectrumView::Handle SpectrumView::handleAt (float x) const
{
    const float xl = xForFreq (proc.apvts.getRawParameterValue ("low")->load());
    const float xh = xForFreq (proc.apvts.getRawParameterValue ("high")->load());
    const float dl = std::abs (x - xl), dh = std::abs (x - xh);
    if (std::min (dl, dh) > 8.0f) return Handle::none;
    return dl <= dh ? Handle::low : Handle::high;
}

void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    const auto h = handleAt ((float) e.x);
    if (h != hover) { hover = h; setMouseCursor (h == Handle::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::LeftRightResizeCursor); }
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    dragging = handleAt ((float) e.x);
    if (dragging != Handle::none)
        proc.apvts.getParameter (dragging == Handle::low ? "low" : "high")->beginChangeGesture();
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging == Handle::none) return;
    auto* p = proc.apvts.getParameter (dragging == Handle::low ? "low" : "high");
    float hz = freqForX ((float) e.x);
    const float other = proc.apvts.getRawParameterValue (dragging == Handle::low ? "high" : "low")->load();
    hz = dragging == Handle::low ? std::min (hz, other) : std::max (hz, other);
    p->setValueNotifyingHost (p->convertTo0to1 (hz));
}

void SpectrumView::mouseUp (const juce::MouseEvent&)
{
    if (dragging != Handle::none)
        proc.apvts.getParameter (dragging == Handle::low ? "low" : "high")->endChangeGesture();
    dragging = Handle::none;
}

//==============================================================================
NagiAudioProcessorEditor::NagiAudioProcessorEditor (NagiAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (spectrum);

    addKnob (depth,       "depth",       "DEPTH",       "Maximum amount of reduction");
    addKnob (sharpness,   "sharpness",   "SHARPNESS",   "Higher = targets narrower resonances");
    addKnob (selectivity, "selectivity", "SELECTIVITY", "Higher = reacts only to more prominent peaks");
    addKnob (attack,      "attack",      "ATTACK",      "How fast reduction kicks in");
    addKnob (release,     "release",     "RELEASE",     "How fast reduction recovers");
    addKnob (low,         "low",         "LOW",         "Lower edge of the processing range");
    addKnob (high,        "high",        "HIGH",        "Upper edge of the processing range");
    addKnob (tilt,        "tilt",        "TILT",        "+ = more sensitive in the highs, - = in the lows");
    addKnob (mix,         "mix",         "MIX",         "Dry / wet");
    addKnob (output,      "output",      "OUTPUT",      "Output gain");

    for (auto* b : { &lowLatencyButton, &highQualityButton })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    lowLatencyButton.setTooltip ("Small FFT: about 5 ms latency. For live singing / streaming.");
    highQualityButton.setTooltip ("Large FFT: finer frequency resolution, about 43 ms latency. For mixing.");

    auto setMode = [this] (int m)
    {
        auto* param = proc.apvts.getParameter ("mode");
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 ((float) m));
        param->endChangeGesture();
    };
    lowLatencyButton.onClick  = [setMode] { setMode (0); };
    highQualityButton.onClick = [setMode] { setMode (1); };

    deltaButton.setName ("delta");
    deltaButton.setClickingTogglesState (true);
    deltaButton.setTooltip ("Listen only to what is being removed");
    addAndMakeVisible (deltaButton);
    deltaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "delta", deltaButton);

    latencyLabel.setFont (uiFont (11.0f));
    latencyLabel.setColour (juce::Label::textColourId, textDim);
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel);

    setSize (980, 540);
    startTimerHz (10);
    timerCallback();
}

NagiAudioProcessorEditor::~NagiAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void NagiAudioProcessorEditor::addKnob (Knob& k, const juce::String& paramId, const juce::String& title, const juce::String& tooltip)
{
    k.slider.setName (paramId);
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 18);
    k.slider.setTooltip (tooltip);
    k.slider.setPopupDisplayEnabled (false, false, nullptr);
    addAndMakeVisible (k.slider);
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, paramId, k.slider);

    k.label.setText (title, juce::dontSendNotification);
    k.label.setFont (uiFont (11.0f, true));
    k.label.setColour (juce::Label::textColourId, textDim);
    k.label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k.label);
}

void NagiAudioProcessorEditor::timerCallback()
{
    const int mode = juce::jlimit (0, 1, (int) proc.apvts.getRawParameterValue ("mode")->load());
    lowLatencyButton.setToggleState (mode == 0, juce::dontSendNotification);
    highQualityButton.setToggleState (mode == 1, juce::dontSendNotification);
    latencyLabel.setText ("latency " + juce::String (proc.getLatencyMs(), 1) + " ms  (" + juce::String (proc.getLatencySamples()) + " smp)",
                          juce::dontSendNotification);
}

void NagiAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    // ロゴ：波紋が静まっていく三本線
    const float lx = 22.0f, ly = 28.0f;
    for (int i = 0; i < 3; ++i)
    {
        juce::Path w;
        const float amp = 5.0f - 2.0f * (float) i;
        w.startNewSubPath (lx, ly + (float) i * 7.0f - 7.0f);
        for (int s = 1; s <= 20; ++s)
            w.lineTo (lx + (float) s * 1.3f, ly + (float) i * 7.0f - 7.0f + amp * std::sin ((float) s * 0.63f));
        g.setColour (accent.withAlpha (1.0f - 0.3f * (float) i));
        g.strokePath (w, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    g.setColour (text);
    g.setFont (uiFont (22.0f, true));
    g.drawText ("NAGI", 56, 14, 80, 28, juce::Justification::centredLeft);
    g.setColour (textDim);
    g.setFont (uiFont (11.0f));
    g.drawText ("resonance suppressor", 124, 20, 160, 18, juce::Justification::centredLeft);

    // ノブ区画
    auto knobArea = getLocalBounds().withTrimmedTop (360).reduced (16, 10).toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (knobArea, 6.0f);

    // グループの区切り線: [DEPTH SHARP SEL] [ATK REL] [LOW HIGH TILT] [MIX OUT]
    const float kw = knobArea.getWidth() / 10.0f;
    g.setColour (panelEdge);
    for (int idx : { 3, 5, 8 })
        g.drawVerticalLine ((int) (knobArea.getX() + kw * (float) idx), knobArea.getY() + 14, knobArea.getBottom() - 14);
}

void NagiAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (56).reduced (16, 12);

    latencyLabel.setBounds (header.removeFromRight (190));
    header.removeFromRight (10);
    deltaButton.setBounds (header.removeFromRight (70));
    header.removeFromRight (16);
    highQualityButton.setBounds (header.removeFromRight (112));
    header.removeFromRight (6);
    lowLatencyButton.setBounds (header.removeFromRight (112));

    spectrum.setBounds (16, 56, getWidth() - 32, 296);

    auto knobArea = getLocalBounds().withTrimmedTop (360).reduced (16, 10).reduced (0, 12);
    const int kw = knobArea.getWidth() / 10;
    for (int i = 0; i < 10; ++i)
    {
        auto cell = juce::Rectangle<int> (knobArea.getX() + i * kw, knobArea.getY(), kw, knobArea.getHeight());
        knobs[i]->label.setBounds (cell.removeFromTop (18));
        knobs[i]->slider.setBounds (cell.reduced (4, 2));
    }
}
