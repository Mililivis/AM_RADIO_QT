#ifndef DSP_FILTERS_H
#define DSP_FILTERS_H

#include "Fft.h"

#include <cstddef>
#include <vector>

namespace dsp
{

/// @brief windowed-sinc low-pass FIR (Blackman window), unity DC gain
/// @param taps odd number of taps
/// @param cutoff cutoff frequency normalised to sample rate (0..0.5)
std::vector<float> designLowpass(size_t taps, float cutoff);

/// @brief FIR filter with real taps, complex data and integer decimation (decim = 1 -> plain filter)
class DecimatingFir
{
public:
    DecimatingFir() = default;
    DecimatingFir(std::vector<float> taps, size_t decim);

    /// @brief filter n input samples, output (about n / decim samples) replaces contents of out
    void process(const cf *in, size_t n, std::vector<cf> &out);
    void reset();

private:
    std::vector<float> taps_;
    size_t decim_ = 1;
    std::vector<cf> buffer_;
    size_t next_ = 0;
};

/// @brief numerically controlled oscillator: multiplies signal by exp(j*2*pi*f*n/fs)
class Nco
{
public:
    void setFrequency(double hz, double sampleRate);
    void mix(const cf *in, size_t n, std::vector<cf> &out);

private:
    std::complex<double> phase_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
};

/// @brief split integer decimation into stages of at most 8 (largest first)
std::vector<unsigned> decimationStages(unsigned factor);

/// @brief multi-stage decimator that keeps |f| < passbandHz free of aliases
class DecimatorChain
{
public:
    DecimatorChain() = default;
    DecimatorChain(double inputRate, unsigned factor, double passbandHz);

    double outputRate() const { return outputRate_; }
    void process(const cf *in, size_t n, std::vector<cf> &out);
    void reset();

private:
    std::vector<DecimatingFir> stages_;
    std::vector<std::vector<cf>> buffers_;
    double outputRate_ = 0.0;
};

/// @brief linear-interpolation resampler for real audio (input already band-limited well below output Nyquist)
class LinearResampler
{
public:
    void configure(double inputRate, double outputRate);
    void process(const float *in, size_t n, std::vector<float> &out);
    void reset();

private:
    double step_ = 1.0;
    double position_ = 0.0;
    float last_ = 0.0f;
    bool passthrough_ = true;
};

} // namespace dsp

#endif
