#ifndef CORE_RTL_SDR_SOURCE_H
#define CORE_RTL_SDR_SOURCE_H

#include "IqSource.h"
#include "RtlSdrDriver.h"

#include <atomic>
#include <chrono>
#include <memory>

namespace core
{

/// @brief IQ source on top of the RTL-SDR driver
class RtlSdrSource : public IqSource
{
public:
    /// @throws sdr::RtlSdrError if the dongle cannot be opened
    explicit RtlSdrSource(uint32_t deviceIndex);
    ~RtlSdrSource() override;

    std::string description() const override;

    bool start(BlockCallback callback, std::string &error) override;
    void stop() override;
    bool isRunning() const override;

    bool isTunable() const override { return true; }
    void setCenterFrequency(double hz) override;
    double centerFrequency() const override { return center_; }

    void setSampleRate(double hz) override;
    double sampleRate() const override { return rate_; }

    /// @param db tuner gain, negative = automatic
    void setGain(double db);
    std::vector<double> supportedGains() const;

    /// @brief direct driver access for the sweep scanner, only while the source is stopped
    sdr::RtlSdrDriver &driver() { return *dev_; }
    /// @brief re-apply own rate and frequency after somebody used driver() directly
    void restoreSettings();

    /// @brief USB buffer size giving ~20 blocks per second
    static uint32_t bufferLengthFor(double sampleRate);

private:
    void onRaw(const uint8_t *data, size_t len);

    std::unique_ptr<sdr::RtlSdrDriver> dev_;
    BlockCallback callback_;
    std::atomic<double> center_{1e6};
    std::atomic<double> rate_{2.4e6};
    uint64_t sequence_ = 0;
    uint64_t samples_ = 0;
};

} // namespace core

#endif
