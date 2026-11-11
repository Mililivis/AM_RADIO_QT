#include "Rebin.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp
{

void rebinPower(const float *power, size_t bins, double startHz, double binHz,
                double fromHz, double toHz, size_t pixels, std::vector<float> &out)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    out.assign(pixels, nan);
    if (bins == 0 || pixels == 0 || binHz <= 0.0 || toHz <= fromHz)
    {
        return;
    }

    const double pixelHz = (toHz - fromHz) / static_cast<double>(pixels);
    const double dataEnd = startHz + binHz * static_cast<double>(bins);

    if (pixelHz < binHz)
    {
        // Zoomed in beyond FFT resolution: each pixel shows the bin under it
        for (size_t p = 0; p < pixels; ++p)
        {
            double f = fromHz + (static_cast<double>(p) + 0.5) * pixelHz;
            if (f < startHz || f >= dataEnd)
            {
                continue;
            }
            auto i = static_cast<size_t>((f - startHz) / binHz);
            out[p] = power[std::min(i, bins - 1)];
        }
        return;
    }

    // Each bin is split between the pixels it overlaps in proportion to the overlap
    std::vector<double> acc(pixels, 0.0);
    std::vector<double> coverage(pixels, 0.0);

    for (size_t i = 0; i < bins; ++i)
    {
        const double b0 = startHz + static_cast<double>(i) * binHz;
        const double b1 = b0 + binHz;
        if (b1 <= fromHz || b0 >= toHz)
        {
            continue;
        }

        const double v = power[i];
        auto p0 = static_cast<long>(std::floor((std::max(b0, fromHz) - fromHz) / pixelHz));
        auto p1 = static_cast<long>(std::floor((std::min(b1, toHz) - fromHz) / pixelHz));
        p0 = std::clamp(p0, 0L, static_cast<long>(pixels) - 1);
        p1 = std::clamp(p1, 0L, static_cast<long>(pixels) - 1);

        for (long p = p0; p <= p1; ++p)
        {
            const double x0 = fromHz + static_cast<double>(p) * pixelHz;
            const double overlap = std::min(b1, x0 + pixelHz) - std::max(b0, x0);
            if (overlap <= 0.0)
            {
                continue;
            }
            if (!std::isnan(v))
            {
                acc[static_cast<size_t>(p)] += v * overlap / binHz;
            }
            coverage[static_cast<size_t>(p)] += overlap;
        }
    }

    for (size_t p = 0; p < pixels; ++p)
    {
        if (coverage[p] > 0.0)
        {
            out[p] = static_cast<float>(acc[p]);
        }
    }
}

} // namespace dsp
