#ifndef CORE_SCANNER_H
#define CORE_SCANNER_H

#include <atomic>
#include <functional>
#include <vector>

namespace sdr
{
class RtlSdrDriver;
}

namespace core
{

struct ScanParams
{
    double startHz = 531e3;
    double stopHz = 1602e3;
    double gridHz = 9e3;
    double thresholdDb = 10.0;   // carrier must be this much above local noise floor
    unsigned averages = 96;      // FFT frames per window: more = cleaner spectrum, slower sweep
};

struct Station
{
    double freqHz;
    float levelDb;   // strongest bin of the carrier, dBFS
    float snrDb;     // carrier above local noise floor
};

struct ScanResult
{
    double startHz = 0.0;         // lower edge of bin 0
    double binHz = 0.0;
    std::vector<float> power;     // linear power per bin (same scaling as SpectrumFrame)
    std::vector<Station> stations;
    double gridHz = 0.0;
};

/// @brief sweep any frequency range with the dongle and build one stitched power spectrum.
///
/// The range is covered by overlapping 2.048 MS/s windows. Every frequency is measured by the clean
/// part of at least one window (the DC spike of RTL2832 and the band edges with filter roll-off
/// are covered by the neighbouring window), so the stitched spectrum has no holes.
/// Inside each window FFT frames overlap by 75 %, i.e. every captured sample is used.
ScanResult runScan(sdr::RtlSdrDriver &dev, const ScanParams &params,
                   const std::atomic<bool> &abort, const std::function<void(int)> &progress);

/// @brief find AM carriers on the channel grid (also used to re-run detection with a new threshold)
std::vector<Station> detectStations(const ScanResult &spectrum, const ScanParams &params);

} // namespace core

#endif
