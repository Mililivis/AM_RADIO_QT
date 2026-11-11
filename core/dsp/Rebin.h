#ifndef DSP_REBIN_H
#define DSP_REBIN_H

#include <cstddef>
#include <vector>

namespace dsp
{

/// @brief energy-conserving resampling of a power spectrum onto display pixels.
///
/// Bin i covers [startHz + i*binHz, startHz + (i+1)*binHz). Each pixel receives the total power
/// of the frequency interval it covers (bins split proportionally at pixel edges), so the sum over
/// pixels equals the sum over bins and no signal disappears when many bins fall into one pixel.
/// When a pixel is narrower than a bin, it shows the full power of the bin under its centre
/// (resolution bandwidth = max(pixel width, bin width)).
/// Pixels outside the data get NaN.
///
/// @param power linear power per bin
/// @param out linear power per pixel (resized to pixels)
void rebinPower(const float *power, size_t bins, double startHz, double binHz,
                double fromHz, double toHz, size_t pixels, std::vector<float> &out);

} // namespace dsp

#endif
