// オフラインDSPテスト: 完全再構成・レイテンシー・共振抑制の効き・CPU時間を確認
#include <juce_dsp/juce_dsp.h>
#include "NagiEngine.h"
#include <cstdio>
#include <random>

static double goertzelDb (const std::vector<float>& x, int start, int len, double freq, double sr)
{
    double re = 0, im = 0, wsum = 0;
    for (int n = 0; n < len; ++n)
    {
        const double w = 0.5 - 0.5 * std::cos (2 * juce::MathConstants<double>::pi * n / len);
        const double ph = 2 * juce::MathConstants<double>::pi * freq * (start + n) / sr;
        re += x[(size_t) (start + n)] * w * std::cos (ph);
        im -= x[(size_t) (start + n)] * w * std::sin (ph);
        wsum += w;
    }
    return 20 * std::log10 (2 * std::sqrt (re * re + im * im) / wsum + 1e-12);
}

static double bandRmsDb (const std::vector<float>& x, int start, int len, double lo, double hi, double sr)
{
    // 粗い帯域エネルギー（FFT）
    const int order = 14, n = 1 << order;
    juce::dsp::FFT fft (order);
    double acc = 0; int frames = 0;
    std::vector<float> buf ((size_t) n * 2);
    for (int s = start; s + n <= start + len; s += n / 2, ++frames)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] = x[(size_t) (s + i)] * (float) (0.5 - 0.5 * std::cos (2 * juce::MathConstants<double>::pi * i / n));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = (int) (lo * n / sr); k <= (int) (hi * n / sr); ++k)
            acc += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    return 10 * std::log10 (acc / std::max (1, frames) + 1e-20);
}

static std::vector<float> run (NagiEngine& e, std::vector<float> in, const NagiSettings& s, double* secs = nullptr)
{
    std::vector<float> r = in;
    const int block = 96;
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (size_t i = 0; i < in.size(); i += block)
    {
        const int len = (int) std::min<size_t> (block, in.size() - i);
        float* chans[2] = { in.data() + i, r.data() + i };
        e.process (chans, 2, len, s);
    }
    if (secs) *secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    return in; // ch0
}

int main()
{
    const double sr = 48000.0;
    const int len = (int) sr * 6;
    int fails = 0;

    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 0.05f);

    // ピンク寄りのノイズ（1次LPFで軽く傾ける）+ 3kHzの強い共振
    std::vector<float> noise ((size_t) len), mixSig ((size_t) len);
    float lp = 0;
    for (int i = 0; i < len; ++i)
    {
        lp = 0.7f * lp + 0.3f * nd (rng);
        noise[(size_t) i] = lp + 0.3f * nd (rng);
        mixSig[(size_t) i] = noise[(size_t) i] + 0.08f * (float) std::sin (2 * juce::MathConstants<double>::pi * 3000.0 * i / sr);
    }

    for (int mode = 0; mode < 2; ++mode)
    {
        const int order = mode == 0 ? 8 : 11;
        const char* name = mode == 0 ? "LowLatency" : "HighQuality";
        NagiEngine e;
        e.prepare (sr, order);
        const int L = e.getLatency();

        // 1) Depth 0 → 入力をLサンプル遅らせたものと一致するか
        NagiSettings flat; flat.depthDb = 0.0f;
        auto out = run (e, mixSig, flat);
        double maxErr = 0;
        for (int i = L + 4096; i < len; ++i)
            maxErr = std::max (maxErr, (double) std::abs (out[(size_t) i] - mixSig[(size_t) (i - L)]));
        std::printf ("[%s] latency=%d samples (%.2f ms), reconstruction max err = %.2e\n", name, L, 1000.0 * L / sr, maxErr);
        if (maxErr > 1e-4) { std::printf ("  FAIL: reconstruction\n"); ++fails; }

        // 2) 共振抑制
        e.reset();
        NagiSettings s; s.depthDb = 12.0f; s.lowHz = 200.0f;
        double secs = 0;
        auto proc = run (e, mixSig, s, &secs);
        const int st = (int) sr * 2, ln = (int) sr * 3;
        const double sineIn  = goertzelDb (mixSig, st - L, ln, 3000.0, sr);
        const double sineOut = goertzelDb (proc,   st,     ln, 3000.0, sr);
        const double bIn  = bandRmsDb (mixSig, st - L, ln, 5000, 15000, sr);
        const double bOut = bandRmsDb (proc,   st,     ln, 5000, 15000, sr);
        const double cIn  = bandRmsDb (mixSig, st - L, ln, 500, 2000, sr);
        const double cOut = bandRmsDb (proc,   st,     ln, 500, 2000, sr);
        std::printf ("  3kHz resonance: %.1f dB -> %.1f dB (%.1f dB)\n", sineIn, sineOut, sineOut - sineIn);
        std::printf ("  5-15kHz noise band change: %.2f dB, 0.5-2kHz: %.2f dB\n", bOut - bIn, cOut - cIn);
        std::printf ("  CPU: %.1f%% of realtime (stereo, 1 core)\n", 100.0 * secs / (len / sr));
        if (sineOut - sineIn > -4.0) { std::printf ("  FAIL: resonance not reduced enough\n"); ++fails; }
        if (std::abs (bOut - bIn) > 2.0 || std::abs (cOut - cIn) > 2.0) { std::printf ("  FAIL: broadband altered too much\n"); ++fails; }

        // 3) Delta: 出力 = 削られた成分。delta + 通常 = 原音
        e.reset();
        NagiSettings d = s; d.delta = true;
        auto del = run (e, mixSig, d);
        e.reset();
        auto norm = run (e, mixSig, s);
        double dErr = 0;
        for (int i = L + 4096; i < len; ++i)
            dErr = std::max (dErr, (double) std::abs (del[(size_t) i] + norm[(size_t) i] - mixSig[(size_t) (i - L)]));
        std::printf ("  delta + wet == dry : max err %.2e\n", dErr);
        if (dErr > 1e-4) { std::printf ("  FAIL: delta\n"); ++fails; }
    }

    std::printf (fails == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", fails);
    return fails == 0 ? 0 : 1;
}
