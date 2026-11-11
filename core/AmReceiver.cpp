#include "AmReceiver.h"

#include "AudioOutput.h"

#include <algorithm>
#include <cmath>

namespace core
{

namespace
{
/// @brief keep the channel away from the band edges where the RTL2832 filter rolls off
constexpr double usableFraction = 0.45;
} // namespace

AmReceiver::AmReceiver(AudioOutput *audio)
    : audio_(audio)
{
}

void AmReceiver::setBandwidth(double hz)
{
    bandwidth_ = hz;
    bandwidthChanged_ = true;
}

void AmReceiver::setVolume(double volume)
{
    volume_ = static_cast<float>(std::clamp(volume, 0.0, 1.0));
}

void AmReceiver::onStreamStart(const StreamInfo &info)
{
    demod_.setBandwidth(bandwidth_);
    demod_.configure(info.sampleRate);
    appliedOffset_ = vfoHz_ - info.centerHz;
    demod_.setOffset(appliedOffset_);
}

void AmReceiver::onBlock(const IqBlockPtr &block)
{
    if (block->sampleRate != demod_.inputRate())
    {
        demod_.configure(block->sampleRate);
    }
    if (bandwidthChanged_.exchange(false))
    {
        demod_.setBandwidth(bandwidth_);
    }

    const double offset = vfoHz_ - block->centerHz;
    if (std::fabs(offset) > usableFraction * block->sampleRate)
    {
        levelDb_ = -120.0f;
        return;   // station is not inside the captured band
    }
    if (offset != appliedOffset_)
    {
        appliedOffset_ = offset;
        demod_.setOffset(offset);
        demod_.reset();
        if (audio_)
        {
            audio_->clear();
        }
    }

    demod_.process(block->iq.data(), block->iq.size(), audioFloat_);
    levelDb_ = demod_.channelLevelDb();

    if (muted_ || !audio_)
    {
        return;
    }
    const float gain = volume_ * 0.9f * 32767.0f;
    audioPcm_.resize(audioFloat_.size());
    for (size_t i = 0; i < audioFloat_.size(); ++i)
    {
        audioPcm_[i] = static_cast<int16_t>(std::clamp(audioFloat_[i] * gain, -32767.0f, 32767.0f));
    }
    audio_->push(audioPcm_.data(), audioPcm_.size());
}

void AmReceiver::onStreamStop()
{
    levelDb_ = -120.0f;
    if (audio_)
    {
        audio_->clear();
    }
}

} // namespace core
