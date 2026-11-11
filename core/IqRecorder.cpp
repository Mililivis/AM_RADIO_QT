#include "IqRecorder.h"

#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace core
{

namespace
{
constexpr size_t writeBufferSize = 4 << 20;

std::string jsonEscape(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::string fileTimestamp(std::chrono::system_clock::time_point t)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%SZ");
    return ss.str();
}
} // namespace

IqRecorder::IqRecorder(std::string directory)
    : directory_(std::move(directory))
{
}

IqRecorder::~IqRecorder()
{
    closeFile();
}

std::string IqRecorder::isoTime(std::chrono::system_clock::time_point t)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return ss.str();
}

RecorderStatus IqRecorder::status() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void IqRecorder::onStreamStart(const StreamInfo &info)
{
    streaming_ = true;
    description_ = info.description;
    expectedSample_ = 0;
}

void IqRecorder::onBlock(const IqBlockPtr &block)
{
    if (!streaming_)
    {
        return;   // stale block after stop
    }

    if (file_ && block->sampleRate != sampleRate_)
    {
        closeFile();
    }
    if (!file_)
    {
        if (!openFile(*block))
        {
            streaming_ = false;
            return;
        }
    }

    if (block->firstSample != expectedSample_ && block->firstSample > expectedSample_)
    {
        uint64_t lost = block->firstSample - expectedSample_;
        gaps_.push_back({written_, lost});
        lost_ += lost;
        writeMeta();
    }
    expectedSample_ = block->firstSample + block->size();

    if (block->centerHz != lastCenter_)
    {
        lastCenter_ = block->centerHz;
        captures_.push_back({written_, block->centerHz, isoTime(block->time)});
        writeMeta();
    }

    if (std::fwrite(block->raw.data(), 1, block->raw.size(), file_) != block->raw.size())
    {
        fail("write failed (disk full?)");
        return;
    }
    written_ += block->raw.size() / 2;
    updateStatus();
}

void IqRecorder::onStreamStop()
{
    closeFile();
    streaming_ = false;
}

bool IqRecorder::openFile(const IqBlock &first)
{
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    std::ostringstream name;
    name << "iq_" << fileTimestamp(first.time) << '_' << std::llround(first.centerHz) << "Hz_"
         << std::llround(first.sampleRate) << "sps";
    basePath_ = (std::filesystem::path(directory_) / name.str()).string();

    file_ = std::fopen((basePath_ + ".sigmf-data").c_str(), "wb");
    if (!file_)
    {
        fail("cannot create " + basePath_ + ".sigmf-data");
        return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, writeBufferSize);

    sampleRate_ = first.sampleRate;
    lastCenter_ = first.centerHz;
    written_ = 0;
    lost_ = 0;
    expectedSample_ = first.firstSample;
    captures_ = {{0, first.centerHz, isoTime(first.time)}};
    gaps_.clear();
    writeMeta();

    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = RecorderStatus{};
    status_.writing = true;
    status_.dataPath = basePath_ + ".sigmf-data";
    return true;
}

void IqRecorder::closeFile()
{
    if (!file_)
    {
        return;
    }
    std::fclose(file_);
    file_ = nullptr;
    writeMeta();
    updateStatus();

    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.writing = false;
}

void IqRecorder::fail(const std::string &message)
{
    if (file_)
    {
        std::fclose(file_);
        file_ = nullptr;
        writeMeta();
    }
    streaming_ = false;
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.writing = false;
    status_.error = message;
}

void IqRecorder::updateStatus()
{
    std::error_code ec;
    auto space = std::filesystem::space(directory_, ec);

    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.samples = written_;
    status_.bytes = written_ * 2;
    status_.lostSamples = lost_;
    status_.seconds = sampleRate_ > 0.0 ? static_cast<double>(written_) / sampleRate_ : 0.0;
    status_.freeBytes = ec ? 0 : space.available;
}

void IqRecorder::writeMeta()
{
    if (basePath_.empty())
    {
        return;
    }

    std::ofstream meta(basePath_ + ".sigmf-meta", std::ios::trunc);
    meta << std::setprecision(15);
    meta << "{\n"
         << "  \"global\": {\n"
         << "    \"core:datatype\": \"cu8\",\n"
         << "    \"core:sample_rate\": " << sampleRate_ << ",\n"
         << "    \"core:version\": \"1.0.0\",\n"
         << "    \"core:hw\": \"" << jsonEscape(description_) << "\",\n"
         << "    \"core:recorder\": \"AM_RADIO_QT\",\n"
         << "    \"core:description\": \"Raw IQ from RTL-SDR, unsigned 8-bit interleaved I/Q (rtl_sdr format)\"\n"
         << "  },\n"
         << "  \"captures\": [\n";
    for (size_t i = 0; i < captures_.size(); ++i)
    {
        const auto &c = captures_[i];
        meta << "    {\"core:sample_start\": " << c.sampleStart << ", \"core:frequency\": " << c.frequency
             << ", \"core:datetime\": \"" << c.datetime << "\"}" << (i + 1 < captures_.size() ? "," : "") << '\n';
    }
    meta << "  ],\n"
         << "  \"annotations\": [\n";
    for (size_t i = 0; i < gaps_.size(); ++i)
    {
        const auto &g = gaps_[i];
        meta << "    {\"core:sample_start\": " << g.sampleStart << ", \"core:sample_count\": 1"
             << ", \"core:comment\": \"data loss: " << g.lost << " samples missing before this point\"}"
             << (i + 1 < gaps_.size() ? "," : "") << '\n';
    }
    meta << "  ]\n"
         << "}\n";
}

} // namespace core
