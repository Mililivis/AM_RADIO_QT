#ifndef DSP_FFT_H
#define DSP_FFT_H

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace dsp
{

using cf = std::complex<float>;

/// @brief forward complex FFT of fixed power-of-two size.
/// Uses FFTW (single precision) when built with HAVE_FFTW, otherwise a built-in radix-2 implementation
class Fft
{
public:
    explicit Fft(size_t size);
    ~Fft();

    Fft(const Fft &) = delete;
    Fft &operator=(const Fft &) = delete;

    size_t size() const { return size_; }

    /// @brief in-place forward transform, data.size() must equal size()
    void forward(std::vector<cf> &data);

    /// @brief name of the backend in use ("FFTW" or "built-in")
    static const char *backend();

private:
    struct Impl;
    size_t size_;
    std::unique_ptr<Impl> impl_;
};

/// @brief Blackman-Harris 4-term window (sidelobes -92 dB)
std::vector<float> blackmanHarris(size_t n);

} // namespace dsp

#endif
