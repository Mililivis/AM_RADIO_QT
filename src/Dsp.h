#ifndef DSP_H
#define DSP_H

#include <complex>
#include <cstddef>
#include <vector>

namespace dsp
{

using cf = std::complex<float>;

/// @brief in-place radix-2 FFT, size must be a power of two
void fft(std::vector<cf> &data);

/// @brief Blackman-Harris window (sidelobes -92 dB), good for spotting weak carriers next to strong ones
std::vector<float> blackmanHarris(size_t n);

/// @brief windowed-sinc low-pass FIR (Blackman window)
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

/// @brief complete AM receiver chain for 1.152 MS/s input:
/// NCO -> FIR /8 (144 kS/s, used for live spectrum) -> FIR /3 (48 kS/s) -> channel filter -> envelope + AGC
class AmDemodulator
{
public:
    static constexpr double inputRate = 1'152'000.0;
    static constexpr double midRate = inputRate / 8.0;   // 144 kS/s
    static constexpr double audioRate = midRate / 3.0;   // 48 kS/s

    AmDemodulator();

    /// @brief shift applied to the input, station must end up at 0 Hz
    void setShift(double hz);
    /// @brief full channel bandwidth (both sidebands), e.g. 9000 Hz
    void setBandwidth(double hz);

    /// @brief process one block of input samples
    /// @param audio demodulated audio in [-1, 1] at audioRate (replaced)
    void process(const cf *in, size_t n, std::vector<float> &audio);

    /// @brief 144 kS/s baseband of the last block, for spectrum display
    const std::vector<cf> &midband() const { return mid_; }

    /// @brief power of the filtered channel of the last block, dBFS
    float channelLevelDb() const { return levelDb_; }

    void reset();

private:
    Nco nco_;
    DecimatingFir stage1_;
    DecimatingFir stage2_;
    DecimatingFir channel_;

    std::vector<cf> mixed_;
    std::vector<cf> mid_;
    std::vector<cf> low_;
    std::vector<cf> chan_;

    float carrier_ = 0.0f;
    float levelDb_ = -120.0f;
};

} // namespace dsp

#endif
