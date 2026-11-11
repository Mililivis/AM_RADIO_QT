#include "FileSource.h"

#include "RtlSdrDriver.h"
#include "RtlSdrSource.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace core
{

namespace
{
bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// @brief value of the first "key": <number> in a JSON text (enough for SigMF written by IqRecorder)
bool findNumber(const std::string &json, const std::string &key, double &value)
{
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos)
    {
        return false;
    }
    // The key itself contains ':' (namespace), look for the separator after it
    pos = json.find(':', pos + quoted.size());
    if (pos == std::string::npos)
    {
        return false;
    }
    value = std::strtod(json.c_str() + pos + 1, nullptr);
    return true;
}
} // namespace

FileSource::FileSource(std::string path, double sampleRate, double centerHz)
    : rate_(sampleRate), center_(centerHz)
{
    const std::string meta = ".sigmf-meta";
    const std::string data = ".sigmf-data";
    if (endsWith(path, meta))
    {
        metaPath_ = path;
        dataPath_ = path.substr(0, path.size() - meta.size()) + data;
    }
    else if (endsWith(path, data))
    {
        dataPath_ = path;
        metaPath_ = path.substr(0, path.size() - data.size()) + meta;
    }
    else
    {
        dataPath_ = path;
    }
}

FileSource::~FileSource()
{
    stop();
}

bool FileSource::open(std::string &error)
{
    if (!metaPath_.empty())
    {
        std::ifstream in(metaPath_);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string json = ss.str();
        if (json.find("\"cu8\"") == std::string::npos)
        {
            error = "only cu8 recordings are supported: " + metaPath_;
            return false;
        }
        findNumber(json, "core:sample_rate", rate_);
        findNumber(json, "core:frequency", center_);
    }

    if (rate_ <= 0.0)
    {
        error = "sample rate unknown for " + dataPath_;
        return false;
    }
    FILE *f = std::fopen(dataPath_.c_str(), "rb");
    if (!f)
    {
        error = "cannot open " + dataPath_;
        return false;
    }
    std::fclose(f);
    return true;
}

std::string FileSource::description() const
{
    return "file " + dataPath_;
}

bool FileSource::start(BlockCallback callback, std::string &error)
{
    if (running_)
    {
        return true;
    }
    if (rate_ <= 0.0)
    {
        error = "sample rate unknown for " + dataPath_;
        return false;
    }
    FILE *f = std::fopen(dataPath_.c_str(), "rb");
    if (!f)
    {
        error = "cannot open " + dataPath_;
        return false;
    }
    std::fclose(f);

    callback_ = std::move(callback);
    running_ = true;
    thread_ = std::thread(&FileSource::run, this);
    return true;
}

void FileSource::stop()
{
    running_ = false;
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void FileSource::run()
{
    FILE *f = std::fopen(dataPath_.c_str(), "rb");
    if (!f)
    {
        running_ = false;
        return;
    }

    const size_t blockBytes = RtlSdrSource::bufferLengthFor(rate_);
    std::vector<uint8_t> buffer(blockBytes);
    uint64_t sequence = 0;
    uint64_t samples = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto wallStart = std::chrono::system_clock::now();

    while (running_)
    {
        size_t n = std::fread(buffer.data(), 1, blockBytes, f);
        n &= ~static_cast<size_t>(1);
        if (n == 0)
        {
            std::rewind(f);   // loop the recording
            continue;
        }

        auto block = std::make_shared<IqBlock>();
        block->sequence = sequence++;
        block->firstSample = samples;
        block->sampleRate = rate_;
        block->centerHz = center_;
        block->time = wallStart + std::chrono::microseconds(static_cast<int64_t>(1e6 * samples / rate_));
        block->raw.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
        sdr::RtlSdrDriver::convertToIq(block->raw.data(), n, block->iq);
        samples += block->iq.size();

        // Real-time pacing
        std::this_thread::sleep_until(start + std::chrono::microseconds(static_cast<int64_t>(1e6 * samples / rate_)));
        if (running_ && callback_)
        {
            callback_(block);
        }
    }
    std::fclose(f);
}

} // namespace core
