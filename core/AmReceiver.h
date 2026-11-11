#ifndef CORE_AM_RECEIVER_H
#define CORE_AM_RECEIVER_H

#include "IqSink.h"
#include "dsp/AmDemodulator.h"

#include <atomic>
#include <vector>

namespace core
{

class AudioOutput;

/// @brief AM demodulator of one station (VFO) anywhere inside the captured band -> audio output
class AmReceiver : public IqSink
{
public:
    explicit AmReceiver(AudioOutput *audio);

    /// @brief absolute frequency of the station, Hz
    void setFrequency(double hz) { vfoHz_ = hz; }
    double frequency() const { return vfoHz_; }
    void setBandwidth(double hz);
    void setVolume(double volume);
    /// @brief muted receiver keeps demodulating (level meter works) but sends no audio
    void setMuted(bool muted) { muted_ = muted; }

    /// @brief channel power, dBFS (-120 when station is outside the captured band)
    float levelDb() const { return levelDb_; }

    void onStreamStart(const StreamInfo &info) override;
    void onBlock(const IqBlockPtr &block) override;
    void onStreamStop() override;

private:
    AudioOutput *audio_;
    dsp::AmDemodulator demod_;

    std::atomic<double> vfoHz_{0.0};
    std::atomic<double> bandwidth_{8000.0};
    std::atomic<bool> bandwidthChanged_{false};
    std::atomic<float> volume_{0.7f};
    std::atomic<bool> muted_{true};
    std::atomic<float> levelDb_{-120.0f};

    double appliedOffset_ = 0.0;
    std::vector<float> audioFloat_;
    std::vector<int16_t> audioPcm_;
};

} // namespace core

#endif
