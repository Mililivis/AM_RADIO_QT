#ifndef SCANNER_H
#define SCANNER_H

#include <atomic>
#include <functional>
#include <vector>

#include <QMetaType>

namespace sdr
{
class RtlSdrDriver;
}

struct ScanParams
{
    double startHz = 531e3;
    double stopHz = 1602e3;
    double gridHz = 9e3;
    double thresholdDb = 10.0;   // carrier must be this much above local noise floor
};

struct Station
{
    double freqHz;
    float levelDb;   // carrier level, dBFS
    float snrDb;     // carrier above local noise floor
};

struct ScanResult
{
    double startHz = 0.0;
    double binHz = 0.0;
    std::vector<float> spectrumDb;   // averaged power spectrum of the whole band
    std::vector<Station> stations;
};

/// @brief sweep the band with the SDR, build averaged spectrum and detect AM carriers on the channel grid
///
/// The band is covered by overlapping 2.048 MS/s windows. Bins near each window centre
/// (DC spike of RTL2832) and near the edges (anti-alias roll-off) are not used,
/// overlap guarantees every frequency is measured by a clean part of some window.
ScanResult runScan(sdr::RtlSdrDriver &dev, const ScanParams &params,
                   const std::atomic<bool> &abort, const std::function<void(int)> &progress);

/// @brief re-run station detection on an existing spectrum (e.g. after changing threshold)
std::vector<Station> detectStations(const ScanResult &spectrum, const ScanParams &params);

Q_DECLARE_METATYPE(ScanResult)

#endif
