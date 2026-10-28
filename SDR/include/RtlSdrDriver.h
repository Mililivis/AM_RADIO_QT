#ifndef RTL_SDR_DRIVER_H
#define RTL_SDR_DRIVER_H

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Forward declaration from <rtl-sdr.h>, so users of the driver do not need librtlsdr headers
typedef struct rtlsdr_dev rtlsdr_dev_t;

namespace sdr
{

using IqSample = std::complex<float>;

/// @brief Exception thrown on any librtlsdr failure
class RtlSdrError : public std::runtime_error
{
public:
    RtlSdrError(const std::string &what, int code = 0);

    /// @brief raw return code of the failed librtlsdr call (negative, usually libusb error)
    int code() const { return code_; }

private:
    int code_;
};

/// @brief Description of an RTL-SDR dongle found on USB
struct DeviceInfo
{
    uint32_t index;
    std::string name;
    std::string manufacturer;
    std::string product;
    std::string serial;
};

enum class GainMode
{
    Auto,   // tuner AGC
    Manual  // fixed gain set with setGain()
};

enum class DirectSampling
{
    Off = 0,
    IBranch = 1,
    QBranch = 2
};

/// @brief C++ driver for RTL2832U based SDR dongles (RTL-SDR Blog V4 etc.) on top of librtlsdr
///
/// Typical usage:
///     sdr::RtlSdrDriver dev(0);
///     dev.setSampleRate(2'048'000);
///     dev.setCenterFrequency(100'000'000);
///     dev.setGainMode(sdr::GainMode::Manual);
///     dev.setGain(30.0);
///     auto iq = dev.readIq(262144);
///
/// The object is not copyable and not movable: the async streaming thread holds a pointer to it.
class RtlSdrDriver
{
public:
    using RawCallback = std::function<void(const uint8_t *data, size_t len)>;
    using IqCallback = std::function<void(const IqSample *samples, size_t count)>;

    // Defaults used by rtl_sdr / rtl_fm
    static constexpr uint32_t defaultSampleRate = 2'048'000;
    static constexpr uint32_t defaultBufferCount = 15;
    static constexpr uint32_t defaultBufferLength = 16 * 16384;

    /// @brief find all dongles connected to USB
    static std::vector<DeviceInfo> listDevices();

    /// @brief open device by index from listDevices()
    explicit RtlSdrDriver(uint32_t index = 0);

    /// @brief open device by its USB serial string (see rtl_eeprom -s to set it)
    static uint32_t indexBySerial(const std::string &serial);

    ~RtlSdrDriver();

    RtlSdrDriver(const RtlSdrDriver &) = delete;
    RtlSdrDriver &operator=(const RtlSdrDriver &) = delete;
    RtlSdrDriver(RtlSdrDriver &&) = delete;
    RtlSdrDriver &operator=(RtlSdrDriver &&) = delete;

    // ---------- Device info ----------
    const DeviceInfo &info() const { return info_; }
    std::string tunerName() const;

    // ---------- Tuning ----------
    /// @brief center frequency in Hz. On V4 frequencies below 28.8 MHz go through the built-in HF upconverter automatically
    void setCenterFrequency(uint32_t hz);
    uint32_t centerFrequency() const;

    /// @brief sample rate in Hz. Valid ranges: 225001..300000 and 900001..3200000 (above 2.56 MHz may drop samples)
    void setSampleRate(uint32_t hz);
    uint32_t sampleRate() const;

    /// @brief crystal error correction in ppm (V4 has TCXO, usually 0..1)
    void setFrequencyCorrection(int ppm);
    int frequencyCorrection() const;

    /// @brief tuner IF filter bandwidth in Hz, 0 = automatic (follows sample rate)
    void setTunerBandwidth(uint32_t hz);

    // ---------- Gain ----------
    void setGainMode(GainMode mode);
    GainMode gainMode() const { return gainMode_; }

    /// @brief set tuner gain in dB, rounded to the nearest supported step. Switches to manual mode
    /// @return gain actually applied in dB
    double setGain(double db);
    double gain() const;

    /// @brief list of gains supported by the tuner in dB
    std::vector<double> supportedGains() const;

    /// @brief RTL2832 digital AGC (separate from tuner AGC)
    void setRtlAgc(bool on);

    // ---------- Misc ----------
    /// @brief 4.5 V bias tee on the antenna port (for LNA / active antennas)
    void setBiasTee(bool on);

    /// @brief direct sampling mode. Not needed on V4 (it has HF upconverter), kept for V3 and clones
    void setDirectSampling(DirectSampling mode);
    DirectSampling directSampling() const;

    /// @brief offset tuning, only for E4000 / FC0012 / FC0013 tuners
    void setOffsetTuning(bool on);

    // ---------- Synchronous reading ----------
    /// @brief read raw interleaved unsigned 8-bit I/Q bytes (I0, Q0, I1, Q1, ...)
    /// @param buffer destination, its size is rounded up to multiple of 512 bytes (USB requirement)
    /// @return number of bytes actually read
    size_t readRaw(std::vector<uint8_t> &buffer);

    /// @brief read count complex samples normalised to [-1, 1]
    std::vector<IqSample> readIq(size_t count);

    /// @brief drop data buffered in the dongle (call after retuning before sync reading)
    void resetBuffer();

    // ---------- Asynchronous streaming ----------
    /// @brief start background thread that calls callback with raw I/Q bytes for every USB buffer
    /// @param bufferLength bytes per buffer, multiple of 512
    void startStreamingRaw(RawCallback callback,
                           uint32_t bufferCount = defaultBufferCount,
                           uint32_t bufferLength = defaultBufferLength);

    /// @brief same as startStreamingRaw but delivers normalised complex samples
    void startStreaming(IqCallback callback,
                        uint32_t bufferCount = defaultBufferCount,
                        uint32_t bufferLength = defaultBufferLength);

    /// @brief stop streaming and join background thread. Safe to call when not streaming
    void stopStreaming();
    bool isStreaming() const { return streaming_; }

    // ---------- Helpers ----------
    /// @brief convert raw unsigned 8-bit I/Q bytes into complex floats in [-1, 1]
    static void convertToIq(const uint8_t *data, size_t len, std::vector<IqSample> &out);

private:
    static void asyncTrampoline(unsigned char *buf, uint32_t len, void *ctx);
    void check(int ret, const std::string &what) const;
    void ensureNotStreaming(const std::string &what) const;

    rtlsdr_dev_t *dev_ = nullptr;
    DeviceInfo info_;
    GainMode gainMode_ = GainMode::Auto;

    std::thread streamThread_;
    std::atomic<bool> streaming_{false};
    std::mutex streamMutex_;
    RawCallback rawCallback_;
    IqCallback iqCallback_;
    std::vector<IqSample> iqBuffer_;
};

} // namespace sdr

#endif
