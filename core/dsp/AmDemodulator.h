#ifndef DSP_AM_DEMODULATOR_H
#define DSP_AM_DEMODULATOR_H

#include "Filters.h"

#include <vector>

namespace dsp
{

/// @brief AM receiver chain for any input sample rate:
/// NCO (station -> 0 Hz) -> integer decimation to >= 48 kS/s -> channel filter
/// -> envelope detector with carrier AGC -> resampling to exactly 48 kS/s
class AmDemodulator
{
public:
    static constexpr double audioRate = 48000.0;

    /// @brief rebuild filters for a new input rate (resets state)
    void configure(double inputRate);
    double inputRate() const { return inputRate_; }

    /// @brief station frequency relative to the centre of the input signal, Hz
    void setOffset(double hz);
    /// @brief full channel bandwidth (both sidebands), e.g. 8000 Hz
    void setBandwidth(double hz);

    /// @param audio demodulated audio in [-1, 1] at audioRate (replaced)
    void process(const cf *in, size_t n, std::vector<float> &audio);

    /// @brief power in the channel filter of the last block, dBFS
    float channelLevelDb() const { return levelDb_; }

    void reset();

private:
    void buildChannelFilter();

    double inputRate_ = 0.0;
    double channelRate_ = 0.0;
    double offset_ = 0.0;
    double bandwidth_ = 8000.0;

    Nco nco_;
    DecimatorChain decimator_;
    DecimatingFir channel_;
    LinearResampler resampler_;

    std::vector<cf> mixed_;
    std::vector<cf> low_;
    std::vector<cf> chan_;
    std::vector<float> envelope_;

    float carrier_ = 0.0f;
    float levelDb_ = -120.0f;
};

} // namespace dsp

#endif
