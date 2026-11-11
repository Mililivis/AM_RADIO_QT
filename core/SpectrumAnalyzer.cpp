#include "SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace core
{

namespace
{
constexpr double targetResolutionHz = 300.0;
constexpr size_t minFftSize = 512;
constexpr size_t maxFftSize = 16384;
constexpr size_t overlapDivider = 4;   // hop = N/4 -> 75 % overlap
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(FrameCallback callback)
    : callback_(std::move(callback))
{
}

size_t SpectrumAnalyzer::fftSizeFor(double sampleRate)
{
    double bits = std::round(std::log2(std::max(1.0, sampleRate / targetResolutionHz)));
    auto size = static_cast<size_t>(1) << static_cast<unsigned>(std::max(0.0, bits));
    return std::clamp(size, minFftSize, maxFftSize);
}

void SpectrumAnalyzer::configure(double sampleRate, double centerHz)
{
    sampleRate_ = sampleRate;
    centerHz_ = centerHz;

    size_t size = fftSizeFor(sampleRate);
    if (size != size_)
    {
        size_ = size;
        hop_ = size / overlapDivider;
        fft_ = std::make_unique<dsp::Fft>(size);
        window_ = dsp::blackmanHarris(size);
        frame_.resize(size);

        double sumSquares = 0.0;
        for (float w : window_)
        {
            sumSquares += static_cast<double>(w) * w;
        }
        // Parseval: sum_k |X_k|^2 = N * sum_n |x_n w_n|^2  ->  sum over bins = mean power
        norm_ = 1.0 / (static_cast<double>(size) * sumSquares);
    }

    pending_.clear();
    accumulator_.assign(size_, 0.0);
    frames_ = 0;
}

void SpectrumAnalyzer::onStreamStart(const StreamInfo &info)
{
    configure(info.sampleRate, info.centerHz);
    expectedSample_ = 0;
}

void SpectrumAnalyzer::onBlock(const IqBlockPtr &block)
{
    // New tuning or lost data: start a clean line (old samples do not belong to the new spectrum)
    if (block->sampleRate != sampleRate_ || block->centerHz != centerHz_ || size_ == 0)
    {
        configure(block->sampleRate, block->centerHz);
    }
    else if (block->firstSample != expectedSample_)
    {
        pending_.clear();
    }
    expectedSample_ = block->firstSample + block->size();

    if (pending_.empty() && frames_ == 0)
    {
        lineTime_ = block->time;
    }
    pending_.insert(pending_.end(), block->iq.begin(), block->iq.end());

    const double rate = std::max(0.5, lineRate_.load());
    const auto framesPerLine = static_cast<uint32_t>(
        std::max(1.0, std::round(sampleRate_ / static_cast<double>(hop_) / rate)));

    size_t pos = 0;
    while (pending_.size() - pos >= size_)
    {
        for (size_t i = 0; i < size_; ++i)
        {
            frame_[i] = pending_[pos + i] * window_[i];
        }
        fft_->forward(frame_);
        for (size_t i = 0; i < size_; ++i)
        {
            accumulator_[i] += std::norm(frame_[i]);
        }
        ++frames_;
        pos += hop_;

        if (frames_ >= framesPerLine)
        {
            emitLine();
            lineTime_ = block->time + std::chrono::microseconds(
                static_cast<int64_t>(1e6 * static_cast<double>(pos) / sampleRate_));
        }
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pos));
}

void SpectrumAnalyzer::onStreamStop()
{
    pending_.clear();
    accumulator_.assign(size_, 0.0);
    frames_ = 0;
}

void SpectrumAnalyzer::emitLine()
{
    auto line = std::make_shared<SpectrumFrame>();
    line->binHz = sampleRate_ / static_cast<double>(size_);
    line->startHz = centerHz_ - sampleRate_ / 2.0 - line->binHz / 2.0;
    line->centerHz = centerHz_;
    line->sampleRate = sampleRate_;
    line->averages = frames_;
    line->time = lineTime_;
    line->power.resize(size_);

    // fftshift: bin 0 = lowest frequency
    const double scale = norm_ / frames_;
    for (size_t i = 0; i < size_; ++i)
    {
        line->power[i] = static_cast<float>(accumulator_[(i + size_ / 2) % size_] * scale);
    }

    std::fill(accumulator_.begin(), accumulator_.end(), 0.0);
    frames_ = 0;

    if (callback_)
    {
        callback_(line);
    }
}

} // namespace core
