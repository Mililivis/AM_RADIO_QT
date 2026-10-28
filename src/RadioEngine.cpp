#include "RadioEngine.h"

#include "AudioOutput.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr size_t liveFftSize = 1024;   // 140 Hz bins at 144 kS/s
constexpr auto spectrumInterval = std::chrono::milliseconds(100);
} // namespace

RadioEngine::RadioEngine(AudioOutput *audio, QObject *parent)
    : QObject(parent), audio_(audio)
{
    qRegisterMetaType<ScanResult>("ScanResult");
    qRegisterMetaType<QVector<float>>("QVector<float>");

    specWindow_ = dsp::blackmanHarris(liveFftSize);
    double sum = 0.0;
    for (float w : specWindow_)
    {
        sum += w;
    }
    specNorm_ = 1.0 / (sum * sum);
    specFrame_.resize(liveFftSize);
    specPower_.assign(liveFftSize, 0.0);
}

RadioEngine::~RadioEngine()
{
    abortScan();
    if (scanThread_.joinable())
    {
        scanThread_.join();
    }
    stop();
}

bool RadioEngine::open(int deviceIndex, QString &error)
{
    try
    {
        dev_ = std::make_unique<sdr::RtlSdrDriver>(static_cast<uint32_t>(deviceIndex));
        dev_->setGainMode(sdr::GainMode::Auto);
        return true;
    }
    catch (const sdr::RtlSdrError &e)
    {
        error = QString::fromStdString(e.what());
        return false;
    }
}

QString RadioEngine::deviceDescription() const
{
    if (!dev_)
    {
        return tr("no device");
    }
    const auto &info = dev_->info();
    return QString("%1 %2, SN %3, %4")
        .arg(QString::fromStdString(info.manufacturer), QString::fromStdString(info.product),
             QString::fromStdString(info.serial), QString::fromStdString(dev_->tunerName()));
}

QVector<double> RadioEngine::supportedGains() const
{
    QVector<double> gains;
    if (dev_)
    {
        for (double g : dev_->supportedGains())
        {
            gains.push_back(g);
        }
    }
    return gains;
}

void RadioEngine::setGain(double db)
{
    if (!dev_ || scanning_)
    {
        return;
    }
    try
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
    catch (const sdr::RtlSdrError &e)
    {
        emit errorOccurred(QString::fromStdString(e.what()));
    }
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void RadioEngine::startScan(const ScanParams &params)
{
    if (!dev_ || scanning_)
    {
        return;
    }
    stop();
    if (scanThread_.joinable())
    {
        scanThread_.join();
    }

    abortScan_ = false;
    scanning_ = true;

    scanThread_ = std::thread([this, params] {
        ScanResult result;
        bool failed = false;
        try
        {
            result = runScan(*dev_, params, abortScan_, [this](int p) { emit scanProgress(p); });
        }
        catch (const sdr::RtlSdrError &e)
        {
            failed = true;
            emit errorOccurred(QString::fromStdString(e.what()));
        }
        scanning_ = false;
        emit scanFinished(result, failed || abortScan_);
    });
}

void RadioEngine::abortScan()
{
    abortScan_ = true;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void RadioEngine::play(double freqHz)
{
    if (!dev_ || scanning_)
    {
        return;
    }

    try
    {
        const auto centre = static_cast<uint32_t>(std::llround(freqHz + tuningOffsetHz));

        if (playing_)
        {
            dev_->setCenterFrequency(centre);
            resetDemod_ = true;
        }
        else
        {
            dev_->setSampleRate(static_cast<uint32_t>(dsp::AmDemodulator::inputRate));
            dev_->setCenterFrequency(centre);

            // Station sits at -offset in baseband, shift it up to 0 Hz
            demod_.setShift(tuningOffsetHz);
            demod_.setBandwidth(bandwidth_);
            demod_.reset();
            specInput_.clear();
            specFrames_ = 0;
            std::fill(specPower_.begin(), specPower_.end(), 0.0);

            dev_->startStreaming([this](const sdr::IqSample *s, size_t n) { onSamples(s, n); });
            playing_ = true;
        }

        freqHz_ = freqHz;
        if (audio_)
        {
            audio_->clear();
        }
        emit playbackChanged(true, freqHz_);
    }
    catch (const sdr::RtlSdrError &e)
    {
        emit errorOccurred(QString::fromStdString(e.what()));
    }
}

void RadioEngine::stop()
{
    if (!dev_ || !playing_)
    {
        return;
    }
    dev_->stopStreaming();
    playing_ = false;
    if (audio_)
    {
        audio_->clear();
    }
    emit playbackChanged(false, freqHz_);
}

bool RadioEngine::isStreamAlive() const
{
    return !playing_ || (dev_ && dev_->isStreaming());
}

void RadioEngine::setBandwidth(double hz)
{
    bandwidth_ = hz;
    bandwidthChanged_ = true;
}

void RadioEngine::setVolume(double volume)
{
    volume_ = static_cast<float>(std::clamp(volume, 0.0, 1.0));
}

void RadioEngine::onSamples(const sdr::IqSample *samples, size_t count)
{
    if (bandwidthChanged_.exchange(false))
    {
        demod_.setBandwidth(bandwidth_);
    }
    if (resetDemod_.exchange(false))
    {
        demod_.reset();
    }

    demod_.process(samples, count, audioFloat_);

    const float gain = volume_ * 0.9f * 32767.0f;
    audioPcm_.resize(audioFloat_.size());
    for (size_t i = 0; i < audioFloat_.size(); ++i)
    {
        audioPcm_[i] = static_cast<int16_t>(std::clamp(audioFloat_[i] * gain, -32767.0f, 32767.0f));
    }
    if (audio_)
    {
        audio_->push(audioPcm_.data(), audioPcm_.size());
    }

    updateSpectrum();
}

void RadioEngine::updateSpectrum()
{
    const auto &mid = demod_.midband();
    specInput_.insert(specInput_.end(), mid.begin(), mid.end());

    size_t used = 0;
    while (specInput_.size() - used >= liveFftSize)
    {
        for (size_t i = 0; i < liveFftSize; ++i)
        {
            specFrame_[i] = specInput_[used + i] * specWindow_[i];
        }
        dsp::fft(specFrame_);
        for (size_t i = 0; i < liveFftSize; ++i)
        {
            specPower_[i] += std::norm(specFrame_[i]);
        }
        ++specFrames_;
        used += liveFftSize;
    }
    specInput_.erase(specInput_.begin(), specInput_.begin() + static_cast<std::ptrdiff_t>(used));

    auto now = std::chrono::steady_clock::now();
    if (specFrames_ == 0 || now - lastSpectrum_ < spectrumInterval)
    {
        return;
    }
    lastSpectrum_ = now;

    // fftshift: lowest frequency first
    QVector<float> db(static_cast<int>(liveFftSize));
    for (size_t i = 0; i < liveFftSize; ++i)
    {
        size_t k = (i + liveFftSize / 2) % liveFftSize;
        db[static_cast<int>(i)] = static_cast<float>(10.0 * std::log10(specPower_[k] / specFrames_ * specNorm_ + 1e-20));
    }
    std::fill(specPower_.begin(), specPower_.end(), 0.0);
    specFrames_ = 0;

    const double bin = dsp::AmDemodulator::midRate / liveFftSize;
    emit liveSpectrum(db, freqHz_ - dsp::AmDemodulator::midRate / 2.0, bin);
    emit levelChanged(demod_.channelLevelDb());
}
