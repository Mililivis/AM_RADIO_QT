#ifndef CORE_IQ_RECORDER_H
#define CORE_IQ_RECORDER_H

#include "IqSink.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace core
{

struct RecorderStatus
{
    bool writing = false;
    std::string dataPath;        // current / last .sigmf-data file
    uint64_t samples = 0;        // IQ samples written to the current file
    uint64_t bytes = 0;
    uint64_t lostSamples = 0;    // samples that did not reach the recorder (queue overflow)
    double seconds = 0.0;
    uint64_t freeBytes = 0;      // free space in the target directory
    std::string error;
};

/// @brief writes the raw IQ stream to disk in SigMF format:
///   <name>.sigmf-data  native cu8 samples, bit-exact copy of what the dongle delivered
///                      (same as rtl_sdr output, opens in GNU Radio, inspectrum, SDR++, URH...)
///   <name>.sigmf-meta  JSON metadata: sample rate, centre frequency per capture segment,
///                      UTC time, hardware; data losses are recorded as annotations
///
/// A new file pair starts with every stream start and whenever the sample rate changes.
/// Retuning inside one stream adds a new capture segment to the metadata.
class IqRecorder : public IqSink
{
public:
    explicit IqRecorder(std::string directory);
    ~IqRecorder() override;

    RecorderStatus status() const;

    void onStreamStart(const StreamInfo &info) override;
    void onBlock(const IqBlockPtr &block) override;
    void onStreamStop() override;

    /// @brief ISO 8601 UTC with milliseconds, e.g. 2026-11-11T18:20:31.123Z
    static std::string isoTime(std::chrono::system_clock::time_point t);

private:
    struct Capture
    {
        uint64_t sampleStart;
        double frequency;
        std::string datetime;
    };
    struct Gap
    {
        uint64_t sampleStart;
        uint64_t lost;
    };

    bool openFile(const IqBlock &first);
    void closeFile();
    void writeMeta();
    void fail(const std::string &message);
    void updateStatus();

    std::string directory_;
    std::string description_;
    bool streaming_ = false;

    FILE *file_ = nullptr;
    std::string basePath_;
    double sampleRate_ = 0.0;
    double lastCenter_ = 0.0;
    uint64_t written_ = 0;
    uint64_t expectedSample_ = 0;
    uint64_t lost_ = 0;
    std::vector<Capture> captures_;
    std::vector<Gap> gaps_;

    mutable std::mutex statusMutex_;
    RecorderStatus status_;
};

} // namespace core

#endif
