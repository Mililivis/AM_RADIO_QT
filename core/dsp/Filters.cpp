#include "Filters.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dsp
{

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr size_t minTaps = 15;
constexpr size_t maxTaps = 255;
} // namespace

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
// DecimatorChain
// ---------------------------------------------------------------------------

std::vector<unsigned> decimationStages(unsigned factor)
{
    std::vector<unsigned> stages;
    while (factor > 1)
    {
        unsigned chosen = factor;   // prime > 8 stays as one stage
        for (unsigned f = 8; f >= 2; --f)
        {
            if (factor % f == 0)
            {
                chosen = f;
                break;
            }
        }
        stages.push_back(chosen);
        factor /= chosen;
    }
    return stages;
}

DecimatorChain::DecimatorChain(double inputRate, unsigned factor, double passbandHz)
{
    double rate = inputRate;
    for (unsigned m : decimationStages(factor))
    {
        const double outRate = rate / m;
        // Aliases fold onto |f| < passband only from |f| > outRate - passband: that is the stop band
        double transition = (outRate - 2.0 * passbandHz) / rate;
        transition = std::max(transition, 0.02);
        size_t taps = static_cast<size_t>(std::ceil(5.5 / transition)) | 1u;
        taps = std::clamp(taps, minTaps, maxTaps);

        stages_.emplace_back(designLowpass(taps, static_cast<float>(0.5 / m)), m);
        rate = outRate;
    }
    buffers_.resize(stages_.size());
    outputRate_ = rate;
}

void DecimatorChain::process(const cf *in, size_t n, std::vector<cf> &out)
{
    if (stages_.empty())
    {
        out.assign(in, in + n);
        return;
    }

    const cf *src = in;
    size_t count = n;
    for (size_t s = 0; s < stages_.size(); ++s)
    {
        std::vector<cf> &dst = (s + 1 == stages_.size()) ? out : buffers_[s];
        stages_[s].process(src, count, dst);
        src = dst.data();
        count = dst.size();
    }
}

void DecimatorChain::reset()
{
    for (auto &s : stages_)
    {
        s.reset();
    }
}

// ---------------------------------------------------------------------------
// LinearResampler
// ---------------------------------------------------------------------------

void LinearResampler::configure(double inputRate, double outputRate)
{
    step_ = inputRate / outputRate;
    passthrough_ = std::fabs(step_ - 1.0) < 1e-9;
    reset();
}

void LinearResampler::reset()
{
    position_ = 0.0;
    last_ = 0.0f;
}

void LinearResampler::process(const float *in, size_t n, std::vector<float> &out)
{
    out.clear();
    if (passthrough_)
    {
        out.assign(in, in + n);
        return;
    }
    if (n == 0)
    {
        return;
    }

    // Extended input: index 0 = last sample of previous block, index i + 1 = in[i]
    auto at = [&](size_t i) { return i == 0 ? last_ : in[i - 1]; };

    out.reserve(static_cast<size_t>(n / step_) + 2);
    double t = position_;
    while (t < static_cast<double>(n))
    {
        size_t i0 = static_cast<size_t>(t);
        float frac = static_cast<float>(t - static_cast<double>(i0));
        out.push_back(at(i0) * (1.0f - frac) + at(i0 + 1) * frac);
        t += step_;
    }
    position_ = t - static_cast<double>(n);
    last_ = in[n - 1];
}

} // namespace dsp
