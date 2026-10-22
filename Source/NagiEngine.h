#pragma once

// Nagi — 動的レゾナンス抑制エンジン（オリジナル実装）
//
// 仕組み:
//   1. STFT（Hann窓・75%オーバーラップ）で周波数ごとに分解
//   2. 各ビンのレベルを、周囲の帯域（対数周波数で一定幅）の平均＝「ベースライン」と比較
//   3. ベースラインより突出しているぶん（共振）だけ、その瞬間に下げる
//   4. アタック／リリースで時間方向をなめらかにし、逆FFTで戻す
//
// レイテンシーはFFTサイズ N サンプルちょうど。

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

struct NagiSettings
{
    float depthDb     = 6.0f;    // 最大削減量 (dB)
    float sharpness   = 4.0f;    // 0..10 高いほど細い共振を狙う
    float selectivity = 4.0f;    // 0..10 高いほど目立つ共振だけに反応
    float attackMs    = 5.0f;
    float releaseMs   = 80.0f;
    float lowHz       = 300.0f;  // 処理範囲
    float highHz      = 20000.0f;
    float tiltDbOct   = 0.0f;    // +で高域ほど敏感に
    float mix         = 1.0f;    // 0..1
    float outGainDb   = 0.0f;
    bool  delta       = false;   // 削った成分だけを聴く
};

// GUI表示用（オーディオスレッド→GUIスレッド、ロックなし）
struct NagiDisplayData
{
    static constexpr int maxBins = 4097;
    std::atomic<int>    numBins { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int>    frameCounter { 0 };
    std::atomic<float>  inputDb[maxBins];
    std::atomic<float>  reductionDb[maxBins];

    NagiDisplayData()
    {
        for (int i = 0; i < maxBins; ++i) { inputDb[i] = -120.0f; reductionDb[i] = 0.0f; }
    }
};

class NagiEngine
{
public:
    static constexpr int maxChannels = 2;

    void prepare (double newSampleRate, int fftOrder)
    {
        sampleRate = newSampleRate;
        N   = 1 << fftOrder;
        hop = N / 4;
        K   = N / 2 + 1;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);

        window.assign ((size_t) N, 0.0f);
        for (int n = 0; n < N; ++n) // periodic Hann
            window[(size_t) n] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) n / (float) N);

        // Hann(分析) × Hann(合成), hop=N/4 の重なり和は 1.5
        olaScale = 1.0f / 1.5f;
        // 片側振幅をdBFS相当に揃える係数（Hann窓の和 = N/2）
        ampScale = 4.0f / (float) N;

        for (int c = 0; c < maxChannels; ++c)
        {
            inFifo[c].assign ((size_t) N, 0.0f);
            outAccum[c].assign ((size_t) N, 0.0f);
            outFifo[c].assign ((size_t) hop, 0.0f);
            spec[c].assign ((size_t) (2 * N), 0.0f);
        }

        level.assign ((size_t) K, -120.0f);
        rawPow.assign ((size_t) K, 0.0f);
        detPow.assign ((size_t) K, 0.0f);
        clipped.assign ((size_t) K, 0.0f);
        base.assign ((size_t) K, 0.0f);
        prefix.assign ((size_t) K + 1, 0.0);
        target.assign ((size_t) K, 0.0f);
        tmp.assign ((size_t) K, 0.0f);
        reduction.assign ((size_t) K, 0.0f);
        gain.assign ((size_t) K, 1.0f);
        loIdx.assign ((size_t) K, 0);
        hiIdx.assign ((size_t) K, 0);
        octave.assign ((size_t) K, 0.0f);

        for (int k = 0; k < K; ++k)
        {
            const float hz = (float) k * (float) sampleRate / (float) N;
            octave[(size_t) k] = std::log2 (std::max (hz, 1.0f) / 1000.0f);
        }

        lastSharpness = -1.0f;
        reset();
    }

    void reset()
    {
        for (int c = 0; c < maxChannels; ++c)
        {
            std::fill (inFifo[c].begin(), inFifo[c].end(), 0.0f);
            std::fill (outAccum[c].begin(), outAccum[c].end(), 0.0f);
            std::fill (outFifo[c].begin(), outFifo[c].end(), 0.0f);
        }
        std::fill (reduction.begin(), reduction.end(), 0.0f);
        std::fill (detPow.begin(), detPow.end(), 0.0f);
        pos = 0;
    }

    int getLatency() const noexcept { return N; }
    int getFftSize() const noexcept { return N; }

    void setDisplay (NagiDisplayData* d) noexcept { display = d; }

    void process (float* const* channels, int numChannels, int numSamples, const NagiSettings& s)
    {
        numChannels = juce::jlimit (1, maxChannels, numChannels);

        for (int i = 0; i < numSamples; ++i)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                inFifo[c][(size_t) (N - hop + pos)] = channels[c][i];
                channels[c][i] = outFifo[c][(size_t) pos];
            }

            if (++pos == hop)
            {
                pos = 0;
                processFrame (numChannels, s);
            }
        }
    }

private:
    void updateBaselineRanges (float sharpness)
    {
        if (std::abs (sharpness - lastSharpness) < 1.0e-4f)
            return;
        lastSharpness = sharpness;

        // ベースライン幅: Sharpness 0 → 2オクターブ, 10 → 1/6オクターブ
        const float widthOct = 2.0f * std::pow (1.0f / 12.0f, juce::jlimit (0.0f, 10.0f, sharpness) / 10.0f);
        const float ratio    = std::pow (2.0f, widthOct * 0.5f);

        for (int k = 1; k < K; ++k)
        {
            int lo = (int) std::floor ((float) k / ratio);
            int hi = (int) std::ceil  ((float) k * ratio);
            lo = std::min (lo, k - 1);
            hi = std::max (hi, k + 1);
            loIdx[(size_t) k] = juce::jlimit (1, K - 1, lo);
            hiIdx[(size_t) k] = juce::jlimit (1, K - 1, hi);
        }
        loIdx[0] = 1; hiIdx[0] = 1;
    }

    void boxMean (const std::vector<float>& src, std::vector<float>& dst)
    {
        prefix[0] = 0.0;
        for (int k = 0; k < K; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + (double) src[(size_t) k];

        for (int k = 0; k < K; ++k)
        {
            const int lo = loIdx[(size_t) k], hi = hiIdx[(size_t) k];
            dst[(size_t) k] = (float) ((prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1));
        }
    }

    void processFrame (int numChannels, const NagiSettings& s)
    {
        // --- 分析 ---
        for (int c = 0; c < numChannels; ++c)
        {
            auto& buf = spec[c];
            for (int n = 0; n < N; ++n)
                buf[(size_t) n] = inFifo[c][(size_t) n] * window[(size_t) n];
            std::fill (buf.begin() + N, buf.end(), 0.0f);
            fft->performRealOnlyForwardTransform (buf.data(), true);
        }

        const float chNorm = 1.0f / (float) numChannels;
        for (int k = 0; k < K; ++k)
        {
            float p = 0.0f;
            for (int c = 0; c < numChannels; ++c)
            {
                const float re = spec[c][(size_t) (2 * k)], im = spec[c][(size_t) (2 * k + 1)];
                p += re * re + im * im;
            }
            rawPow[(size_t) k] = p * chNorm * ampScale * ampScale;
        }

        // 検出用レベル: 隣接ビンと短時間で平均してノイズのばらつきを抑える
        // （ばらつきが大きいと、ノイズのランダムな山まで「共振」と誤検出してしまう）
        const float aDet = 1.0f - std::exp (-((float) hop / (float) sampleRate) / 0.008f);
        for (int k = 0; k < K; ++k)
        {
            const float pl = rawPow[(size_t) std::max (0, k - 1)];
            const float pr = rawPow[(size_t) std::min (K - 1, k + 1)];
            const float pf = 0.25f * pl + 0.5f * rawPow[(size_t) k] + 0.25f * pr;
            float& ps = detPow[(size_t) k];
            ps += aDet * (pf - ps);
            level[(size_t) k] = 10.0f * std::log10 (ps + 1.0e-14f);
        }

        // --- ベースライン（2段階: 突出部分を頭打ちにして再平均 → 共振に引っ張られにくい）---
        updateBaselineRanges (s.sharpness);
        boxMean (level, base);
        for (int k = 0; k < K; ++k)
            clipped[(size_t) k] = std::min (level[(size_t) k], base[(size_t) k]);
        boxMean (clipped, base);

        // --- 目標削減量 ---
        const float selThr   = 1.0f + juce::jlimit (0.0f, 10.0f, s.selectivity) * 1.2f; // 1..13 dB
        const float lowOct   = std::log2 (std::max (s.lowHz, 1.0f) / 1000.0f);
        const float highOct  = std::log2 (std::max (s.highHz, 1.0f) / 1000.0f);
        const float edgeOct  = 1.0f / 3.0f;
        const float depth    = std::max (0.0f, s.depthDb);

        target[0] = 0.0f;
        for (int k = 1; k < K; ++k)
        {
            const float oct = octave[(size_t) k];
            const float thr = juce::jlimit (0.0f, 30.0f, selThr - s.tiltDbOct * oct);
            const float over = level[(size_t) k] - base[(size_t) k] - thr;

            const float wl = juce::jlimit (0.0f, 1.0f, (oct - lowOct)  / edgeOct + 0.5f);
            const float wh = juce::jlimit (0.0f, 1.0f, (highOct - oct) / edgeOct + 0.5f);
            const float w  = wl * wh;

            float r = 0.0f;
            if (over > 0.0f && w > 0.0f && level[(size_t) k] > -80.0f)
                r = depth * w * (1.0f - std::exp (-over / 6.0f));
            target[(size_t) k] = r;
        }

        // 周波数方向に軽くならす（ミュージカルノイズ対策）
        for (int pass = 0; pass < 2; ++pass)
        {
            tmp[0] = target[0];
            tmp[(size_t) K - 1] = target[(size_t) K - 1];
            for (int k = 1; k < K - 1; ++k)
                tmp[(size_t) k] = 0.25f * target[(size_t) k - 1] + 0.5f * target[(size_t) k] + 0.25f * target[(size_t) k + 1];
            std::swap (tmp, target);
        }

        // --- 時間方向（アタック／リリース）---
        const float dt = (float) hop / (float) sampleRate;
        const float aA = 1.0f - std::exp (-dt / (std::max (0.1f, s.attackMs)  * 0.001f));
        const float aR = 1.0f - std::exp (-dt / (std::max (1.0f, s.releaseMs) * 0.001f));

        const float out = juce::Decibels::decibelsToGain (s.outGainDb);
        const float mix = juce::jlimit (0.0f, 1.0f, s.mix);

        for (int k = 0; k < K; ++k)
        {
            float& r = reduction[(size_t) k];
            const float t = target[(size_t) k];
            r += (t > r ? aA : aR) * (t - r);

            const float g = std::pow (10.0f, -r / 20.0f);
            float gm = s.delta ? (1.0f - g) : (1.0f - mix * (1.0f - g));
            gain[(size_t) k] = gm * out;
        }

        // --- 合成 ---
        for (int c = 0; c < numChannels; ++c)
        {
            auto& buf = spec[c];
            for (int k = 0; k < K; ++k)
            {
                buf[(size_t) (2 * k)]     *= gain[(size_t) k];
                buf[(size_t) (2 * k + 1)] *= gain[(size_t) k];
            }
            fft->performRealOnlyInverseTransform (buf.data());

            auto& acc = outAccum[c];
            for (int n = 0; n < N; ++n)
                acc[(size_t) n] += buf[(size_t) n] * window[(size_t) n] * olaScale;

            std::copy (acc.begin(), acc.begin() + hop, outFifo[c].begin());
            std::move (acc.begin() + hop, acc.end(), acc.begin());
            std::fill (acc.end() - hop, acc.end(), 0.0f);

            auto& in = inFifo[c];
            std::move (in.begin() + hop, in.end(), in.begin());
        }

        // --- 表示用 ---
        if (display != nullptr)
        {
            const int nb = std::min (K, NagiDisplayData::maxBins);
            for (int k = 0; k < nb; ++k)
            {
                display->inputDb[k].store (level[(size_t) k], std::memory_order_relaxed);
                display->reductionDb[k].store (reduction[(size_t) k], std::memory_order_relaxed);
            }
            display->sampleRate.store (sampleRate, std::memory_order_relaxed);
            display->numBins.store (nb, std::memory_order_relaxed);
            display->frameCounter.fetch_add (1, std::memory_order_release);
        }
    }

    double sampleRate = 48000.0;
    int N = 256, hop = 64, K = 129, pos = 0;
    float olaScale = 1.0f, ampScale = 1.0f, lastSharpness = -1.0f;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window;
    std::vector<float> inFifo[maxChannels], outAccum[maxChannels], outFifo[maxChannels], spec[maxChannels];
    std::vector<float> level, rawPow, detPow, clipped, base, target, tmp, reduction, gain, octave;
    std::vector<double> prefix;
    std::vector<int> loIdx, hiIdx;

    NagiDisplayData* display = nullptr;
};
