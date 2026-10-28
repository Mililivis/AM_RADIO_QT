#include "AudioOutput.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <unistd.h>
#include <vector>

namespace
{
constexpr unsigned chunkMs = 20;
constexpr unsigned prebufferMs = 120;
constexpr unsigned maxBufferMs = 600;
constexpr unsigned trimToMs = 200;
} // namespace

AudioOutput::AudioOutput(Backend backend, unsigned sampleRate, int stdoutFd)
    : backend_(backend), rate_(sampleRate), fd_(stdoutFd)
{
}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::start(std::string &error)
{
    if (running_)
    {
        return true;
    }

    if (backend_ == Backend::Pulse)
    {
        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_S16LE;
        spec.rate = rate_;
        spec.channels = 1;

        pa_buffer_attr attr{};
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.tlength = static_cast<uint32_t>(pa_usec_to_bytes(100'000, &spec));
        attr.prebuf = static_cast<uint32_t>(-1);
        attr.minreq = static_cast<uint32_t>(-1);
        attr.fragsize = static_cast<uint32_t>(-1);

        int err = 0;
        pulse_ = pa_simple_new(nullptr, "AM Radio", PA_STREAM_PLAYBACK, nullptr, "AM Radio",
                               &spec, nullptr, &attr, &err);
        if (!pulse_)
        {
            error = std::string("PulseAudio: ") + pa_strerror(err);
            return false;
        }
    }

    starved_ = true;
    running_ = true;
    thread_ = std::thread(&AudioOutput::run, this);
    return true;
}

void AudioOutput::stop()
{
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable())
    {
        thread_.join();
    }
    if (pulse_)
    {
        pa_simple_free(pulse_);
        pulse_ = nullptr;
    }
}

void AudioOutput::push(const int16_t *samples, size_t count)
{
    if (!running_)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.insert(queue_.end(), samples, samples + count);

        if (queue_.size() > rate_ * maxBufferMs / 1000)
        {
            size_t drop = queue_.size() - rate_ * trimToMs / 1000;
            queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }
    cv_.notify_one();
}

void AudioOutput::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    starved_ = true;
}

unsigned AudioOutput::bufferedMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<unsigned>(queue_.size() * 1000 / rate_);
}

std::string AudioOutput::backendName() const
{
    return backend_ == Backend::Pulse ? "PulseAudio" : "stdout (s16le mono " + std::to_string(rate_) + " Hz)";
}

void AudioOutput::run()
{
    const size_t chunk = rate_ * chunkMs / 1000;
    const size_t prebuffer = rate_ * prebufferMs / 1000;
    std::vector<int16_t> data;

    while (running_)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [&] {
                return !running_ || queue_.size() >= (starved_ ? prebuffer : 1);
            });
            if (!running_)
            {
                break;
            }
            if (queue_.empty())
            {
                starved_ = true;
                continue;
            }
            if (starved_ && queue_.size() < prebuffer)
            {
                continue;
            }
            starved_ = false;

            size_t n = std::min(chunk, queue_.size());
            data.assign(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
            queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
        }

        if (!write(data.data(), data.size()))
        {
            running_ = false;
        }
    }
}

bool AudioOutput::write(const int16_t *data, size_t count)
{
    const size_t bytes = count * sizeof(int16_t);

    if (backend_ == Backend::Pulse)
    {
        int err = 0;
        return pa_simple_write(pulse_, data, bytes, &err) >= 0;
    }

    const char *p = reinterpret_cast<const char *>(data);
    size_t left = bytes;
    while (left > 0)
    {
        ssize_t n = ::write(fd_, p, left);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;   // EPIPE: player on the laptop was closed
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}
