#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

namespace
{
constexpr double pi = 3.14159265358979323846;
}

void fft(std::vector<cf> &data)
{
    const size_t n = data.size();

    // Bit reversal permutation
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(data[i], data[j]);
        }
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * pi / static_cast<double>(len);
        const cf wLen(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        for (size_t i = 0; i < n; i += len)
        {
            cf w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                cf u = data[i + k];
                cf v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wLen;
            }
        }
    }
}

std::vector<float> blackmanHarris(size_t n)
{
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i)
    {
        double x = 2.0 * pi * static_cast<double>(i) / static_cast<double>(n - 1);
        w[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x)
                                  - 0.01168 * std::cos(3 * x));
    }
    return w;
}

std::vector<float> designLowpass(size_t taps, float cutoff)
{
    std::vector<float> h(taps);
    const double m = static_cast<double>(taps - 1) / 2.0;
    double sum = 0.0;

    for (size_t i = 0; i < taps; ++i)
    {
        double x = static_cast<double>(i) - m;
        double sinc = (x == 0.0) ? 2.0 * cutoff : std::sin(2.0 * pi * cutoff * x) / (pi * x);
        double window = 0.42 - 0.5 * std::cos(2.0 * pi * i / (taps - 1)) + 0.08 * std::cos(4.0 * pi * i / (taps - 1));
        h[i] = static_cast<float>(sinc * window);
        sum += h[i];
    }

    // Unity gain at DC
    for (auto &v : h)
    {
        v = static_cast<float>(v / sum);
    }
    return h;
}

// ---------------------------------------------------------------------------
// DecimatingFir
// ---------------------------------------------------------------------------

DecimatingFir::DecimatingFir(std::vector<float> taps, size_t decim)
    : taps_(std::move(taps)), decim_(decim)
{
    reset();
}

void DecimatingFir::reset()
{
    buffer_.assign(taps_.empty() ? 0 : taps_.size() - 1, cf(0.0f, 0.0f));
    next_ = buffer_.size();
}

void DecimatingFir::process(const cf *in, size_t n, std::vector<cf> &out)
{
    out.clear();
    if (taps_.empty())
    {
        out.assign(in, in + n);
        return;
    }

    const size_t history = taps_.size() - 1;
    buffer_.insert(buffer_.end(), in, in + n);
    out.reserve(n / decim_ + 1);

    const float *t = taps_.data();
    const size_t count = taps_.size();
    size_t i = next_;

    // i = index of the newest sample used by the next output
    for (; i < buffer_.size(); i += decim_)
    {
        const float *x = reinterpret_cast<const float *>(&buffer_[i - history]);
        float re = 0.0f;
        float im = 0.0f;
        for (size_t k = 0; k < count; ++k)
        {
            re += t[k] * x[2 * k];
            im += t[k] * x[2 * k + 1];
        }
        out.emplace_back(re, im);
    }

    const size_t drop = buffer_.size() - history;
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
    next_ = i - drop;
}

// ---------------------------------------------------------------------------
// Nco
// ---------------------------------------------------------------------------

void Nco::setFrequency(double hz, double sampleRate)
{
    step_ = std::polar(1.0, 2.0 * pi * hz / sampleRate);
}

void Nco::mix(const cf *in, size_t n, std::vector<cf> &out)
{
    out.resize(n);
    std::complex<double> p = phase_;
    for (size_t i = 0; i < n; ++i)
    {
        out[i] = in[i] * cf(static_cast<float>(p.real()), static_cast<float>(p.imag()));
        p *= step_;
    }
    phase_ = p / std::abs(p);
}

// ---------------------------------------------------------------------------
// AmDemodulator
// ---------------------------------------------------------------------------

AmDemodulator::AmDemodulator()
    : stage1_(designLowpass(127, 0.05f), 8),
      stage2_(designLowpass(63, 0.13f), 3)
{
    setBandwidth(9000.0);
}

void AmDemodulator::setShift(double hz)
{
    nco_.setFrequency(hz, inputRate);
}

void AmDemodulator::setBandwidth(double hz)
{
    channel_ = DecimatingFir(designLowpass(151, static_cast<float>(hz / 2.0 / audioRate)), 1);
}

void AmDemodulator::reset()
{
    stage1_.reset();
    stage2_.reset();
    channel_.reset();
    carrier_ = 0.0f;
}

void AmDemodulator::process(const cf *in, size_t n, std::vector<float> &audio)
{
    nco_.mix(in, n, mixed_);
    stage1_.process(mixed_.data(), mixed_.size(), mid_);
    stage2_.process(mid_.data(), mid_.size(), low_);
    channel_.process(low_.data(), low_.size(), chan_);

    // Envelope detector with carrier-based AGC (time constant ~100 ms)
    constexpr float alpha = 1.0f / (0.1f * static_cast<float>(audioRate));
    audio.resize(chan_.size());
    double power = 0.0;

    for (size_t i = 0; i < chan_.size(); ++i)
    {
        float mag = std::abs(chan_[i]);
        power += static_cast<double>(mag) * mag;

        if (carrier_ <= 0.0f)
        {
            carrier_ = mag;
        }
        carrier_ += alpha * (mag - carrier_);

        float a = (mag - carrier_) / std::max(carrier_, 1e-6f);
        audio[i] = std::clamp(a, -1.0f, 1.0f);
    }

    if (!chan_.empty())
    {
        levelDb_ = static_cast<float>(10.0 * std::log10(power / chan_.size() + 1e-20));
    }
}

} // namespace dsp
