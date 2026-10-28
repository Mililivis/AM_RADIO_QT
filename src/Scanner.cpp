#include "Scanner.h"

#include "Dsp.h"
#include "RtlSdrDriver.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

constexpr uint32_t scanRate = 2'048'000;
constexpr size_t fftSize = 4096;                 // 500 Hz bins
constexpr size_t averages = 96;                  // ~190 ms of signal per window
constexpr size_t settleSamples = 65536;          // dropped after retune (PLL + AGC settle)
constexpr double usableHalf = 0.4 * scanRate;    // ±819 kHz around window centre
constexpr double dcGuard = 6e3;                  // ignore DC spike
constexpr double margin = 20e3;                  // extra spectrum shown around band

constexpr double carrierSearchHz = 1e3;          // carrier may be slightly off grid
constexpr double floorWindowHz = 100e3;          // noise floor estimated in ±100 kHz
constexpr double floorPercentile = 0.2;
constexpr double sidebandFromHz = 2e3;
constexpr double sidebandToHz = 4e3;
constexpr float minCarrierOverSidebandsDb = 3.0f;

const float noData = std::numeric_limits<float>::quiet_NaN();

std::vector<double> windowCentres(double gridStart, double gridStop, double bin)
{
    const double step = std::floor(0.85 * usableHalf / bin) * bin;
    std::vector<double> centres;

    double c = std::round((gridStart + usableHalf) / bin) * bin;
    centres.push_back(c);

    // Whole band fits into the left half of the first window
    if (gridStop <= c - dcGuard)
    {
        return centres;
    }

    // Every next window covers DC of the previous one with its left half
    while (centres.back() + usableHalf < gridStop || centres.size() < 2)
    {
        centres.push_back(centres.back() + step);
    }
    return centres;
}

} // namespace

ScanResult runScan(sdr::RtlSdrDriver &dev, const ScanParams &params,
                   const std::atomic<bool> &abort, const std::function<void(int)> &progress)
{
    const double bin = static_cast<double>(scanRate) / fftSize;

    ScanResult result;
    result.binHz = bin;
    result.startHz = std::max(0.0, std::floor((params.startHz - margin) / bin) * bin);
    const double gridStop = params.stopHz + margin;
    const size_t nBins = static_cast<size_t>((gridStop - result.startHz) / bin) + 1;

    std::vector<double> sum(nBins, 0.0);
    std::vector<uint32_t> count(nBins, 0);

    const std::vector<float> window = dsp::blackmanHarris(fftSize);
    double windowSum = 0.0;
    for (float w : window)
    {
        windowSum += w;
    }
    // full-scale complex tone -> 0 dB
    const double norm = 1.0 / (windowSum * windowSum);

    dev.setSampleRate(scanRate);

    const std::vector<double> centres = windowCentres(result.startHz, gridStop, bin);
    std::vector<dsp::cf> frame(fftSize);
    std::vector<double> power(fftSize);

    for (size_t w = 0; w < centres.size() && !abort; ++w)
    {
        const double centre = centres[w];
        dev.setCenterFrequency(static_cast<uint32_t>(centre));
        dev.resetBuffer();
        dev.readIq(settleSamples);

        std::vector<sdr::IqSample> iq = dev.readIq(fftSize * averages);
        std::fill(power.begin(), power.end(), 0.0);
        size_t frames = iq.size() / fftSize;

        for (size_t f = 0; f < frames; ++f)
        {
            for (size_t i = 0; i < fftSize; ++i)
            {
                frame[i] = iq[f * fftSize + i] * window[i];
            }
            dsp::fft(frame);
            for (size_t i = 0; i < fftSize; ++i)
            {
                power[i] += std::norm(frame[i]);
            }
        }

        if (frames > 0)
        {
            for (size_t k = 0; k < fftSize; ++k)
            {
                double offset = (k < fftSize / 2 ? static_cast<double>(k)
                                                  : static_cast<double>(k) - fftSize) * bin;
                if (std::fabs(offset) < dcGuard || std::fabs(offset) > usableHalf)
                {
                    continue;
                }
                long idx = std::lround((centre + offset - result.startHz) / bin);
                if (idx < 0 || static_cast<size_t>(idx) >= nBins)
                {
                    continue;
                }
                sum[static_cast<size_t>(idx)] += power[k] / frames * norm;
                count[static_cast<size_t>(idx)] += 1;
            }
        }

        if (progress)
        {
            progress(static_cast<int>((w + 1) * 100 / centres.size()));
        }
    }

    result.spectrumDb.resize(nBins);
    for (size_t i = 0; i < nBins; ++i)
    {
        result.spectrumDb[i] = count[i] ? static_cast<float>(10.0 * std::log10(sum[i] / count[i] + 1e-20))
                                        : noData;
    }

    result.stations = detectStations(result, params);
    return result;
}

std::vector<Station> detectStations(const ScanResult &spectrum, const ScanParams &params)
{
    std::vector<Station> stations;
    const auto &db = spectrum.spectrumDb;
    if (db.empty() || spectrum.binHz <= 0.0 || params.gridHz <= 0.0)
    {
        return stations;
    }

    const double bin = spectrum.binHz;
    const long n = static_cast<long>(db.size());
    const long searchBins = std::lround(carrierSearchHz / bin);
    const long floorBins = std::lround(floorWindowHz / bin);
    const long sbFrom = std::lround(sidebandFromHz / bin);
    const long sbTo = std::lround(sidebandToHz / bin);

    auto valid = [&](long i) { return i >= 0 && i < n && !std::isnan(db[static_cast<size_t>(i)]); };

    std::vector<float> neighbourhood;
    double first = std::ceil(params.startHz / params.gridHz - 1e-9) * params.gridHz;

    for (double f = first; f <= params.stopHz + 1e-6; f += params.gridHz)
    {
        long ci = std::lround((f - spectrum.startHz) / bin);

        // Carrier = strongest bin near the grid frequency
        float carrier = -std::numeric_limits<float>::infinity();
        long carrierIdx = -1;
        for (long i = ci - searchBins; i <= ci + searchBins; ++i)
        {
            if (valid(i) && db[static_cast<size_t>(i)] > carrier)
            {
                carrier = db[static_cast<size_t>(i)];
                carrierIdx = i;
            }
        }
        if (carrierIdx < 0)
        {
            continue;
        }

        // Local noise floor = low percentile of surrounding bins (robust against other stations)
        neighbourhood.clear();
        for (long i = ci - floorBins; i <= ci + floorBins; ++i)
        {
            if (valid(i))
            {
                neighbourhood.push_back(db[static_cast<size_t>(i)]);
            }
        }
        if (neighbourhood.size() < 10)
        {
            continue;
        }
        auto nth = neighbourhood.begin() + static_cast<std::ptrdiff_t>(neighbourhood.size() * floorPercentile);
        std::nth_element(neighbourhood.begin(), nth, neighbourhood.end());
        float floorDb = *nth;

        // AM carrier is a narrow line above its own sidebands; wideband noise is flat
        double sbPower = 0.0;
        int sbCount = 0;
        for (long d = sbFrom; d <= sbTo; ++d)
        {
            for (long i : {carrierIdx - d, carrierIdx + d})
            {
                if (valid(i))
                {
                    sbPower += std::pow(10.0, db[static_cast<size_t>(i)] / 10.0);
                    ++sbCount;
                }
            }
        }
        float sidebandDb = sbCount ? static_cast<float>(10.0 * std::log10(sbPower / sbCount)) : floorDb;

        float snr = carrier - floorDb;
        if (snr >= params.thresholdDb && carrier - sidebandDb >= minCarrierOverSidebandsDb)
        {
            stations.push_back({f, carrier, snr});
        }
    }

    return stations;
}
