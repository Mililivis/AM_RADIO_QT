#include "RtlSdrDriver.h"

#include <rtl-sdr.h>

#include <algorithm>
#include <cmath>

namespace sdr
{

namespace
{

constexpr size_t usbChunk = 512;

size_t roundUpToChunk(size_t len)
{
    return ((len + usbChunk - 1) / usbChunk) * usbChunk;
}

/// @brief lookup table u8 -> float, 127.4 is the real DC midpoint of RTL2832 ADC
const std::array<float, 256> &conversionTable()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (size_t i = 0; i < t.size(); ++i)
        {
            t[i] = (static_cast<float>(i) - 127.4f) / 128.0f;
        }
        return t;
    }();
    return table;
}

std::string openErrorHint(int ret)
{
    switch (ret)
    {
    case -3: // LIBUSB_ERROR_ACCESS
        return " (permission denied: install udev rules or run with sudo, see README)";
    case -6: // LIBUSB_ERROR_BUSY
        return " (device busy: kernel DVB driver dvb_usb_rtl28xxu or another program holds it, see README)";
    default:
        return "";
    }
}

} // namespace

RtlSdrError::RtlSdrError(const std::string &what, int code)
    : std::runtime_error(what + " (code " + std::to_string(code) + ")"), code_(code)
{
}

// ---------------------------------------------------------------------------
// Device enumeration / lifetime
// ---------------------------------------------------------------------------

std::vector<DeviceInfo> RtlSdrDriver::listDevices()
{
    std::vector<DeviceInfo> devices;
    uint32_t count = rtlsdr_get_device_count();

    for (uint32_t i = 0; i < count; ++i)
    {
        char manufacturer[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};

        DeviceInfo info;
        info.index = i;
        const char *name = rtlsdr_get_device_name(i);
        info.name = name ? name : "";
        if (rtlsdr_get_device_usb_strings(i, manufacturer, product, serial) == 0)
        {
            info.manufacturer = manufacturer;
            info.product = product;
            info.serial = serial;
        }
        devices.push_back(info);
    }

    return devices;
}

uint32_t RtlSdrDriver::indexBySerial(const std::string &serial)
{
    int index = rtlsdr_get_index_by_serial(serial.c_str());
    if (index < 0)
    {
        throw RtlSdrError("No RTL-SDR device with serial '" + serial + "'", index);
    }
    return static_cast<uint32_t>(index);
}

RtlSdrDriver::RtlSdrDriver(uint32_t index)
{
    if (rtlsdr_get_device_count() == 0)
    {
        throw RtlSdrError("No RTL-SDR devices found on USB", 0);
    }

    int ret = rtlsdr_open(&dev_, index);
    if (ret < 0 || dev_ == nullptr)
    {
        dev_ = nullptr;
        throw RtlSdrError("Failed to open RTL-SDR #" + std::to_string(index) + openErrorHint(ret), ret);
    }

    info_.index = index;
    const char *name = rtlsdr_get_device_name(index);
    info_.name = name ? name : "";

    char manufacturer[256] = {0};
    char product[256] = {0};
    char serial[256] = {0};
    if (rtlsdr_get_usb_strings(dev_, manufacturer, product, serial) == 0)
    {
        info_.manufacturer = manufacturer;
        info_.product = product;
        info_.serial = serial;
    }

    try
    {
        setSampleRate(defaultSampleRate);
        setGainMode(GainMode::Auto);
        resetBuffer();
    }
    catch (...)
    {
        rtlsdr_close(dev_);
        dev_ = nullptr;
        throw;
    }
}

RtlSdrDriver::~RtlSdrDriver()
{
    stopStreaming();
    if (streamThread_.joinable())
    {
        // stopStreaming() was called from the stream callback earlier, finish the join here
        streamThread_.join();
    }
    if (dev_)
    {
        rtlsdr_close(dev_);
    }
}

std::string RtlSdrDriver::tunerName() const
{
    switch (rtlsdr_get_tuner_type(dev_))
    {
    case RTLSDR_TUNER_E4000:  return "Elonics E4000";
    case RTLSDR_TUNER_FC0012: return "Fitipower FC0012";
    case RTLSDR_TUNER_FC0013: return "Fitipower FC0013";
    case RTLSDR_TUNER_FC2580: return "FCI FC2580";
    case RTLSDR_TUNER_R820T:  return "Rafael Micro R820T/R860";
    case RTLSDR_TUNER_R828D:  return "Rafael Micro R828D";
    default:                  return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

void RtlSdrDriver::setCenterFrequency(uint32_t hz)
{
    check(rtlsdr_set_center_freq(dev_, hz), "set center frequency " + std::to_string(hz) + " Hz");
}

uint32_t RtlSdrDriver::centerFrequency() const
{
    return rtlsdr_get_center_freq(dev_);
}

void RtlSdrDriver::setSampleRate(uint32_t hz)
{
    ensureNotStreaming("set sample rate");
    check(rtlsdr_set_sample_rate(dev_, hz), "set sample rate " + std::to_string(hz) + " Hz");
}

uint32_t RtlSdrDriver::sampleRate() const
{
    return rtlsdr_get_sample_rate(dev_);
}

void RtlSdrDriver::setFrequencyCorrection(int ppm)
{
    // librtlsdr returns -2 when the value is unchanged, that is not an error
    int ret = rtlsdr_set_freq_correction(dev_, ppm);
    if (ret == -2)
    {
        return;
    }
    check(ret, "set frequency correction " + std::to_string(ppm) + " ppm");
}

int RtlSdrDriver::frequencyCorrection() const
{
    return rtlsdr_get_freq_correction(dev_);
}

void RtlSdrDriver::setTunerBandwidth(uint32_t hz)
{
    check(rtlsdr_set_tuner_bandwidth(dev_, hz), "set tuner bandwidth " + std::to_string(hz) + " Hz");
}

// ---------------------------------------------------------------------------
// Gain
// ---------------------------------------------------------------------------

void RtlSdrDriver::setGainMode(GainMode mode)
{
    check(rtlsdr_set_tuner_gain_mode(dev_, mode == GainMode::Manual ? 1 : 0), "set gain mode");
    gainMode_ = mode;
}

double RtlSdrDriver::setGain(double db)
{
    std::vector<double> gains = supportedGains();
    if (gains.empty())
    {
        throw RtlSdrError("Tuner reports no supported gains", 0);
    }

    double nearest = *std::min_element(gains.begin(), gains.end(), [db](double a, double b) {
        return std::fabs(a - db) < std::fabs(b - db);
    });

    if (gainMode_ != GainMode::Manual)
    {
        setGainMode(GainMode::Manual);
    }
    // librtlsdr works in tenths of dB
    check(rtlsdr_set_tuner_gain(dev_, static_cast<int>(std::lround(nearest * 10.0))), "set gain");

    return nearest;
}

double RtlSdrDriver::gain() const
{
    return rtlsdr_get_tuner_gain(dev_) / 10.0;
}

std::vector<double> RtlSdrDriver::supportedGains() const
{
    int count = rtlsdr_get_tuner_gains(dev_, nullptr);
    if (count <= 0)
    {
        return {};
    }

    std::vector<int> raw(static_cast<size_t>(count));
    count = rtlsdr_get_tuner_gains(dev_, raw.data());

    std::vector<double> gains;
    for (int i = 0; i < count; ++i)
    {
        gains.push_back(raw[static_cast<size_t>(i)] / 10.0);
    }
    return gains;
}

void RtlSdrDriver::setRtlAgc(bool on)
{
    check(rtlsdr_set_agc_mode(dev_, on ? 1 : 0), "set RTL AGC");
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void RtlSdrDriver::setBiasTee(bool on)
{
    check(rtlsdr_set_bias_tee(dev_, on ? 1 : 0), "set bias tee");
}

void RtlSdrDriver::setDirectSampling(DirectSampling mode)
{
    check(rtlsdr_set_direct_sampling(dev_, static_cast<int>(mode)), "set direct sampling");
}

DirectSampling RtlSdrDriver::directSampling() const
{
    int mode = rtlsdr_get_direct_sampling(dev_);
    check(mode, "get direct sampling");
    return static_cast<DirectSampling>(mode);
}

void RtlSdrDriver::setOffsetTuning(bool on)
{
    check(rtlsdr_set_offset_tuning(dev_, on ? 1 : 0), "set offset tuning");
}

// ---------------------------------------------------------------------------
// Synchronous reading
// ---------------------------------------------------------------------------

void RtlSdrDriver::resetBuffer()
{
    ensureNotStreaming("reset buffer");
    check(rtlsdr_reset_buffer(dev_), "reset buffer");
}

size_t RtlSdrDriver::readRaw(std::vector<uint8_t> &buffer)
{
    ensureNotStreaming("read samples");

    buffer.resize(roundUpToChunk(std::max<size_t>(buffer.size(), usbChunk)));

    int nRead = 0;
    check(rtlsdr_read_sync(dev_, buffer.data(), static_cast<int>(buffer.size()), &nRead), "read samples");

    buffer.resize(static_cast<size_t>(nRead));
    return buffer.size();
}

std::vector<IqSample> RtlSdrDriver::readIq(size_t count)
{
    std::vector<uint8_t> raw(count * 2);
    readRaw(raw);

    std::vector<IqSample> iq;
    convertToIq(raw.data(), raw.size(), iq);
    if (iq.size() > count)
    {
        iq.resize(count);
    }
    return iq;
}

// ---------------------------------------------------------------------------
// Asynchronous streaming
// ---------------------------------------------------------------------------

void RtlSdrDriver::startStreamingRaw(RawCallback callback, uint32_t bufferCount, uint32_t bufferLength)
{
    std::lock_guard<std::mutex> lock(streamMutex_);

    if (streaming_ || streamThread_.joinable())
    {
        throw RtlSdrError("Streaming already running", 0);
    }
    if (bufferLength == 0 || bufferLength % usbChunk != 0)
    {
        throw RtlSdrError("Buffer length must be a non-zero multiple of 512", 0);
    }

    check(rtlsdr_reset_buffer(dev_), "reset buffer");

    rawCallback_ = std::move(callback);
    streaming_ = true;

    streamThread_ = std::thread([this, bufferCount, bufferLength] {
        // Blocks until rtlsdr_cancel_async() or USB error
        rtlsdr_read_async(dev_, &RtlSdrDriver::asyncTrampoline, this, bufferCount, bufferLength);
        streaming_ = false;
    });
}

void RtlSdrDriver::startStreaming(IqCallback callback, uint32_t bufferCount, uint32_t bufferLength)
{
    iqCallback_ = std::move(callback);
    iqBuffer_.reserve(bufferLength / 2);

    startStreamingRaw([this](const uint8_t *data, size_t len) {
        convertToIq(data, len, iqBuffer_);
        iqCallback_(iqBuffer_.data(), iqBuffer_.size());
    }, bufferCount, bufferLength);
}

void RtlSdrDriver::stopStreaming()
{
    std::lock_guard<std::mutex> lock(streamMutex_);

    if (!streamThread_.joinable())
    {
        return;
    }

    rtlsdr_cancel_async(dev_);

    // Called from inside the callback: cannot join ourselves, destructor or next stop will join
    if (std::this_thread::get_id() == streamThread_.get_id())
    {
        return;
    }

    streamThread_.join();
    streaming_ = false;
}

void RtlSdrDriver::asyncTrampoline(unsigned char *buf, uint32_t len, void *ctx)
{
    auto *self = static_cast<RtlSdrDriver *>(ctx);
    if (self->streaming_ && self->rawCallback_)
    {
        self->rawCallback_(buf, len);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void RtlSdrDriver::convertToIq(const uint8_t *data, size_t len, std::vector<IqSample> &out)
{
    const auto &table = conversionTable();
    size_t count = len / 2;

    out.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        out[i] = IqSample(table[data[2 * i]], table[data[2 * i + 1]]);
    }
}

void RtlSdrDriver::check(int ret, const std::string &what) const
{
    if (ret < 0)
    {
        throw RtlSdrError("RTL-SDR: failed to " + what, ret);
    }
}

void RtlSdrDriver::ensureNotStreaming(const std::string &what) const
{
    if (streaming_)
    {
        throw RtlSdrError("RTL-SDR: cannot " + what + " while streaming", 0);
    }
}

} // namespace sdr
