#include "RtlSdrSource.h"

#include <algorithm>
#include <cmath>

namespace core
{

namespace
{
constexpr double blocksPerSecond = 20.0;
constexpr uint32_t usbChunk = 16384;
} // namespace

RtlSdrSource::RtlSdrSource(uint32_t deviceIndex)
    : dev_(std::make_unique<sdr::RtlSdrDriver>(deviceIndex))
{
    dev_->setSampleRate(static_cast<uint32_t>(rate_.load()));
    dev_->setCenterFrequency(static_cast<uint32_t>(center_.load()));
    dev_->setGainMode(sdr::GainMode::Auto);
}

RtlSdrSource::~RtlSdrSource()
{
    stop();
}

std::string RtlSdrSource::description() const
{
    const auto &info = dev_->info();
    return info.manufacturer + " " + info.product + ", SN " + info.serial + ", " + dev_->tunerName();
}

uint32_t RtlSdrSource::bufferLengthFor(double sampleRate)
{
    double bytes = sampleRate * 2.0 / blocksPerSecond;
    auto chunks = static_cast<uint32_t>(std::max(1.0, std::floor(bytes / usbChunk)));
    return chunks * usbChunk;
}

bool RtlSdrSource::start(BlockCallback callback, std::string &error)
{
    if (dev_->isStreaming())
    {
        return true;
    }
    callback_ = std::move(callback);
    sequence_ = 0;
    samples_ = 0;
    try
    {
        dev_->startStreamingRaw([this](const uint8_t *d, size_t n) { onRaw(d, n); },
                                sdr::RtlSdrDriver::defaultBufferCount, bufferLengthFor(rate_));
        return true;
    }
    catch (const sdr::RtlSdrError &e)
    {
        error = e.what();
        return false;
    }
}

void RtlSdrSource::stop()
{
    dev_->stopStreaming();
}

bool RtlSdrSource::isRunning() const
{
    return dev_->isStreaming();
}

void RtlSdrSource::setCenterFrequency(double hz)
{
    dev_->setCenterFrequency(static_cast<uint32_t>(std::llround(hz)));
    center_ = hz;
}

void RtlSdrSource::setSampleRate(double hz)
{
    if (hz == rate_)
    {
        return;
    }
    // librtlsdr cannot change the rate while streaming: restart with the same consumer
    const bool running = dev_->isStreaming();
    BlockCallback callback = callback_;
    if (running)
    {
        stop();
    }
    dev_->setSampleRate(static_cast<uint32_t>(std::llround(hz)));
    rate_ = hz;
    if (running)
    {
        std::string error;
        start(callback, error);
    }
}

void RtlSdrSource::restoreSettings()
{
    dev_->setSampleRate(static_cast<uint32_t>(std::llround(rate_.load())));
    dev_->setCenterFrequency(static_cast<uint32_t>(std::llround(center_.load())));
}

void RtlSdrSource::setGain(double db)
{
    if (db < 0.0)
    {
        dev_->setGainMode(sdr::GainMode::Auto);
    }
    else
    {
        dev_->setGain(db);
    }
}

std::vector<double> RtlSdrSource::supportedGains() const
{
    return dev_->supportedGains();
}

void RtlSdrSource::onRaw(const uint8_t *data, size_t len)
{
    auto block = std::make_shared<IqBlock>();
    block->sequence = sequence_++;
    block->firstSample = samples_;
    block->sampleRate = rate_;
    block->centerHz = center_;
    block->raw.assign(data, data + len);
    sdr::RtlSdrDriver::convertToIq(data, len, block->iq);

    // Callback fires when the buffer is full: first sample is one buffer duration ago
    const double seconds = static_cast<double>(block->iq.size()) / block->sampleRate;
    block->time = std::chrono::system_clock::now()
                  - std::chrono::microseconds(static_cast<int64_t>(seconds * 1e6));
    samples_ += block->iq.size();

    if (callback_)
    {
        callback_(block);
    }
}

} // namespace core
