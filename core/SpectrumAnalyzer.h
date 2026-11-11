#ifndef CORE_SPECTRUM_ANALYZER_H
#define CORE_SPECTRUM_ANALYZER_H

#include "IqSink.h"
#include "dsp/Fft.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace core
{

/// @brief one spectrum line (for the spectrum plot and one row of the waterfall)
struct SpectrumFrame
{
    double startHz = 0.0;        // lower edge of bin 0
    double binHz = 0.0;          // bin i covers [startHz + i*binHz, startHz + (i+1)*binHz)
    std::vector<float> power;    // linear power per bin; sum over bins = mean signal power,
                                 // a full-scale complex tone has power 1.0 (0 dBFS)
    double centerHz = 0.0;
    double sampleRate = 0.0;
    uint32_t averages = 0;       // FFT frames averaged into this line
    bool sweep = false;          // stitched from a sweep (panorama) instead of one live capture
    std::chrono::system_clock::time_point time;

    double stopHz() const { return startHz + binHz * static_cast<double>(power.size()); }
};

using SpectrumFramePtr = std::shared_ptr<const SpectrumFrame>;

/// @brief live power spectrum of the whole captured band.
///
/// Nothing is thrown away: every sample takes part (Blackman-Harris window with 75 % overlap,
/// the overlapped windows sum to an almost flat envelope), all FFT frames between two output
/// lines are averaged, and the scaling is power-conserving (Parseval), so the sum of bins
/// equals the signal power and wideband signals are displayed with their full energy.
class SpectrumAnalyzer : public IqSink
{
public:
    using FrameCallback = std::function<void(const SpectrumFramePtr &)>;

    explicit SpectrumAnalyzer(FrameCallback callback);

    /// @brief output lines per second (waterfall speed)
    void setLineRate(double linesPerSecond) { lineRate_ = linesPerSecond; }

    /// @brief FFT size giving ~300 Hz resolution at the given sample rate
    static size_t fftSizeFor(double sampleRate);

    void onStreamStart(const StreamInfo &info) override;
    void onBlock(const IqBlockPtr &block) override;
    void onStreamStop() override;

private:
    void configure(double sampleRate, double centerHz);
    void emitLine();

    FrameCallback callback_;
    std::atomic<double> lineRate_{10.0};

    double sampleRate_ = 0.0;
    double centerHz_ = 0.0;
    size_t size_ = 0;
    size_t hop_ = 0;
    std::unique_ptr<dsp::Fft> fft_;
    std::vector<float> window_;
    double norm_ = 1.0;

    std::vector<cf> pending_;
    std::vector<cf> frame_;
    std::vector<double> accumulator_;
    uint32_t frames_ = 0;
    uint64_t expectedSample_ = 0;
    std::chrono::system_clock::time_point lineTime_;
};

} // namespace core

#endif
