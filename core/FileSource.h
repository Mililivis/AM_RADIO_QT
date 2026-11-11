#ifndef CORE_FILE_SOURCE_H
#define CORE_FILE_SOURCE_H

#include "IqSource.h"

#include <atomic>
#include <thread>

namespace core
{

/// @brief replays a cu8 recording in real time (SigMF from IqRecorder or plain rtl_sdr file), looping.
/// Lets the whole application (spectrum, waterfall, receiver, network) work without a dongle.
class FileSource : public IqSource
{
public:
    /// @param path .sigmf-meta / .sigmf-data (rate and frequency read from metadata)
    ///             or raw cu8 file (then sampleRate and centerHz must be given)
    FileSource(std::string path, double sampleRate = 0.0, double centerHz = 0.0);
    ~FileSource() override;

    /// @brief check the file and read metadata
    bool open(std::string &error);

    std::string description() const override;

    bool start(BlockCallback callback, std::string &error) override;
    void stop() override;
    bool isRunning() const override { return running_; }

    bool isTunable() const override { return false; }
    void setCenterFrequency(double) override {}
    double centerFrequency() const override { return center_; }
    void setSampleRate(double) override {}
    double sampleRate() const override { return rate_; }

private:
    void run();

    std::string dataPath_;
    std::string metaPath_;
    double rate_;
    double center_;

    BlockCallback callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace core

#endif
