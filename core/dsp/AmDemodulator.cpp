#include "AmDemodulator.h"

#include <algorithm>
#include <cmath>

namespace dsp
{

namespace
{
constexpr double decimatorPassbandHz = 12e3;   // widest channel is 10 kHz -> ±5 kHz, with margin
constexpr size_t channelTaps = 151;
constexpr float agcTimeConstant = 0.1f;        // seconds
} // namespace

void AmDemodulator::configure(double inputRate)
{
    inputRate_ = inputRate;
    auto factor = static_cast<unsigned>(std::max(1.0, std::floor(inputRate / audioRate)));
    decimator_ = DecimatorChain(inputRate, factor, decimatorPassbandHz);
    channelRate_ = decimator_.outputRate();
    resampler_.configure(channelRate_, audioRate);
    buildChannelFilter();
    setOffset(offset_);
    reset();
}

void AmDemodulator::setOffset(double hz)
{
    offset_ = hz;
    if (inputRate_ > 0.0)
    {
        nco_.setFrequency(-hz, inputRate_);
    }
}

void AmDemodulator::setBandwidth(double hz)
{
    bandwidth_ = hz;
    if (channelRate_ > 0.0)
    {
        buildChannelFilter();
    }
}

void AmDemodulator::buildChannelFilter()
{
    channel_ = DecimatingFir(designLowpass(channelTaps, static_cast<float>(bandwidth_ / 2.0 / channelRate_)), 1);
}

void AmDemodulator::reset()
{
    decimator_.reset();
    channel_.reset();
    resampler_.reset();
    carrier_ = 0.0f;
}

void AmDemodulator::process(const cf *in, size_t n, std::vector<float> &audio)
{
    audio.clear();
    if (inputRate_ <= 0.0)
    {
        return;
    }

    nco_.mix(in, n, mixed_);
    decimator_.process(mixed_.data(), mixed_.size(), low_);
    channel_.process(low_.data(), low_.size(), chan_);

    const float alpha = 1.0f / (agcTimeConstant * static_cast<float>(channelRate_));
    envelope_.resize(chan_.size());
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

        // Modulation relative to carrier: AGC and DC removal in one step
        float a = (mag - carrier_) / std::max(carrier_, 1e-6f);
        envelope_[i] = std::clamp(a, -1.0f, 1.0f);
    }

    if (!chan_.empty())
    {
        levelDb_ = static_cast<float>(10.0 * std::log10(power / chan_.size() + 1e-20));
    }

    resampler_.process(envelope_.data(), envelope_.size(), audio);
}

} // namespace dsp
