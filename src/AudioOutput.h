#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct pa_simple;

/// @brief mono 16-bit audio sink with its own thread and jitter buffer
///
/// Backends:
///  - Pulse:  PulseAudio / PipeWire on the Pi (local speakers or PULSE_SERVER tunnel)
///  - Stdout: raw s16le PCM to a file descriptor, piped over SSH into pw-cat / aplay on the laptop
///
/// SDR clock and sound card clock are never exactly equal, so the buffer drops
/// old samples when it grows too long and re-prebuffers after an underrun.
class AudioOutput
{
public:
    enum class Backend
    {
        Pulse,
        Stdout
    };

    AudioOutput(Backend backend, unsigned sampleRate, int stdoutFd = 1);
    ~AudioOutput();

    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    /// @return false and fills error if the backend cannot be opened
    bool start(std::string &error);
    void stop();

    /// @brief called from DSP thread, never blocks
    void push(const int16_t *samples, size_t count);

    /// @brief drop everything buffered (e.g. on station change)
    void clear();

    unsigned bufferedMs() const;
    std::string backendName() const;
    bool isRunning() const { return running_; }

private:
    void run();
    bool write(const int16_t *data, size_t count);

    Backend backend_;
    unsigned rate_;
    int fd_;
    pa_simple *pulse_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<int16_t> queue_;
    bool starved_ = true;
};

#endif
