#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>

#ifdef HAVE_FFTW
#include <fftw3.h>
#endif

namespace dsp
{

namespace
{
constexpr double pi = 3.14159265358979323846;

#ifdef HAVE_FFTW
/// @brief FFTW planner is not thread-safe
std::mutex &plannerMutex()
{
    static std::mutex mutex;
    return mutex;
}
#endif
} // namespace

#ifdef HAVE_FFTW

struct Fft::Impl
{
    fftwf_complex *buffer = nullptr;
    fftwf_plan plan = nullptr;
};

Fft::Fft(size_t size)
    : size_(size), impl_(std::make_unique<Impl>())
{
    std::lock_guard<std::mutex> lock(plannerMutex());

    impl_->buffer = fftwf_alloc_complex(size);
    impl_->plan = fftwf_plan_dft_1d(static_cast<int>(size), impl_->buffer, impl_->buffer, FFTW_FORWARD, FFTW_ESTIMATE);
}

Fft::~Fft()
{
    std::lock_guard<std::mutex> lock(plannerMutex());
    fftwf_destroy_plan(impl_->plan);
    fftwf_free(impl_->buffer);
}

void Fft::forward(std::vector<cf> &data)
{
    auto *buf = reinterpret_cast<cf *>(impl_->buffer);
    std::copy(data.begin(), data.end(), buf);
    fftwf_execute(impl_->plan);
    std::copy(buf, buf + size_, data.begin());
}

const char *Fft::backend()
{
    return "FFTW";
}

#else

struct Fft::Impl
{
    std::vector<cf> twiddles;          // exp(-j*2*pi*k/n), k < n/2
    std::vector<uint32_t> bitReverse;
};

Fft::Fft(size_t size)
    : size_(size), impl_(std::make_unique<Impl>())
{
    if (size < 2 || (size & (size - 1)) != 0)
    {
        throw std::invalid_argument("FFT size must be a power of two");
    }

    impl_->twiddles.resize(size / 2);
    for (size_t k = 0; k < size / 2; ++k)
    {
        double a = -2.0 * pi * static_cast<double>(k) / static_cast<double>(size);
        impl_->twiddles[k] = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }

    unsigned bits = 0;
    while ((size_t(1) << bits) < size)
    {
        ++bits;
    }
    impl_->bitReverse.resize(size);
    for (size_t i = 0; i < size; ++i)
    {
        uint32_t r = 0;
        for (unsigned b = 0; b < bits; ++b)
        {
            r |= ((i >> b) & 1u) << (bits - 1 - b);
        }
        impl_->bitReverse[i] = r;
    }
}

Fft::~Fft() = default;

void Fft::forward(std::vector<cf> &data)
{
    const size_t n = size_;
    for (size_t i = 0; i < n; ++i)
    {
        size_t j = impl_->bitReverse[i];
        if (i < j)
        {
            std::swap(data[i], data[j]);
        }
    }

    float *d = reinterpret_cast<float *>(data.data());
    const cf *tw = impl_->twiddles.data();

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const size_t half = len / 2;
        const size_t step = n / len;
        for (size_t i = 0; i < n; i += len)
        {
            for (size_t k = 0; k < half; ++k)
            {
                const float wr = tw[k * step].real();
                const float wi = tw[k * step].imag();
                float *a = d + 2 * (i + k);
                float *b = d + 2 * (i + k + half);
                const float vr = b[0] * wr - b[1] * wi;
                const float vi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - vr;
                b[1] = a[1] - vi;
                a[0] += vr;
                a[1] += vi;
            }
        }
    }
}

const char *Fft::backend()
{
    return "built-in";
}

#endif

std::vector<float> blackmanHarris(size_t n)
{
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i)
    {
        // Periodic form: overlapped frames sum to an almost flat envelope
        double x = 2.0 * pi * static_cast<double>(i) / static_cast<double>(n);
        w[i] = static_cast<float>(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x)
                                  - 0.01168 * std::cos(3 * x));
    }
    return w;
}

} // namespace dsp
