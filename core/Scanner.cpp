#include "Scanner.h"

#include "RtlSdrDriver.h"
#include "dsp/Fft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{

namespace
{

constexpr uint32_t scanRate = 2'048'000;
constexpr size_t fftSize = 4096;                 // 500 Hz bins
constexpr size_t hop = fftSize / 4;              // 75 % overlap
constexpr size_t settleSamples = 65536;          // dropped after retune (PLL + AGC settle)
constexpr double usableHalf = 0.4 * scanRate;    // ±819 kHz around window centre
constexpr double dcGuard = 6e3;                  // DC spike excluded, covered by neighbour window
constexpr double margin = 20e3;                  // extra spectrum shown around the range

constexpr double carrierSearchHz = 1e3;          // carrier may be slightly off grid
constexpr double floorWindowHz = 100e3;          // noise floor estimated in ±100 kHz
constexpr double floorPercentile = 0.2;
constexpr double sidebandFromHz = 2e3;
constexpr double sidebandToHz = 4e3;
constexpr float minCarrierOverSidebandsDb = 3.0f;

std::vector<double> windowCentres(double firstBin, double lastBin, double bin)
{
    const double step = std::floor(0.85 * usableHalf / bin) * bin;
    std::vector<double> centres;

    double c = std::round((firstBin + usableHalf) / bin) * bin;
    centres.push_back(c);

    // Whole range fits into the left half of the first window
    if (lastBin <= c - dcGuard)
    {
        return centres;
    }

    // Every next window covers DC of the previous one with its left half
    while (centres.back() + usableHalf < lastBin || centres.size() < 2)
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

    // Output grid: bin i is centred at firstBin + i*bin
    const double firstBin = std::max(bin, std::floor((params.startHz - margin) / bin) * bin);
    const double lastBin = params.stopHz + margin;
    const size_t nBins = static_cast<size_t>((lastBin - firstBin) / bin) + 1;

    ScanResult result;
    result.binHz = bin;
    result.startHz = firstBin - bin / 2.0;
    result.gridHz = params.gridHz;

    std::vector<double> sum(nBins, 0.0);
    std::vector<uint32_t> count(nBins, 0);

    const std::vector<float> window = dsp::blackmanHarris(fftSize);
    double sumSquares = 0.0;
    for (float w : window)
    {
        sumSquares += static_cast<double>(w) * w;
    }
    const double norm = 1.0 / (static_cast<double>(fftSize) * sumSquares);   // power-conserving

    dev.setSampleRate(scanRate);

    dsp::Fft fft(fftSize);
    const std::vector<double> centres = windowCentres(firstBin, lastBin, bin);
    std::vector<dsp::cf> frame(fftSize);
    std::vector<double> power(fftSize);
    const size_t samplesPerWindow = fftSize + hop * (std::max(1u, params.averages) - 1);

    for (size_t w = 0; w < centres.size() && !abort; ++w)
    {
        const double centre = centres[w];
        dev.setCenterFrequency(static_cast<uint32_t>(centre));
        dev.resetBuffer();
        dev.readIq(settleSamples);

        std::vector<sdr::IqSample> iq = dev.readIq(samplesPerWindow);
        std::fill(power.begin(), power.end(), 0.0);
        size_t frames = 0;

        for (size_t pos = 0; pos + fftSize <= iq.size(); pos += hop)
        {
            for (size_t i = 0; i < fftSize; ++i)
            {
                frame[i] = iq[pos + i] * window[i];
            }
            fft.forward(frame);
            for (size_t i = 0; i < fftSize; ++i)
            {
                power[i] += std::norm(frame[i]);
            }
            ++frames;
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
                long idx = std::lround((centre + offset - firstBin) / bin);
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

    result.power.resize(nBins);
    for (size_t i = 0; i < nBins; ++i)
    {
        result.power[i] = count[i] ? static_cast<float>(sum[i] / count[i])
                                   : std::numeric_limits<float>::quiet_NaN();
    }

    result.stations = detectStations(result, params);
    return result;
}

std::vector<Station> detectStations(const ScanResult &spectrum, const ScanParams &params)
{
    std::vector<Station> stations;
    if (spectrum.power.empty() || spectrum.binHz <= 0.0 || params.gridHz <= 0.0)
    {
        return stations;
    }

    std::vector<float> db(spectrum.power.size());
    for (size_t i = 0; i < db.size(); ++i)
    {
        float p = spectrum.power[i];
        db[i] = std::isnan(p) ? p : static_cast<float>(10.0 * std::log10(p + 1e-20));
    }

    const double bin = spectrum.binHz;
    const double firstBin = spectrum.startHz + bin / 2.0;
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
        long ci = std::lround((f - firstBin) / bin);

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

} // namespace core
