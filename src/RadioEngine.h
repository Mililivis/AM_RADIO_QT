#ifndef RADIO_ENGINE_H
#define RADIO_ENGINE_H

#include "Dsp.h"
#include "RtlSdrDriver.h"
#include "Scanner.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class AudioOutput;


/// @brief owns the SDR and runs either band scan (own thread) or AM playback (librtlsdr stream thread)
///
/// All public methods are called from the GUI thread. Signals are emitted from worker threads
/// and arrive in the GUI thread as queued connections.
class RadioEngine : public QObject
{
    Q_OBJECT

public:
    /// @brief SDR is tuned this much above the station so its DC spike stays out of the channel
    static constexpr double tuningOffsetHz = 200e3;

    explicit RadioEngine(AudioOutput *audio, QObject *parent = nullptr);
    ~RadioEngine() override;

    bool open(int deviceIndex, QString &error);
    QString deviceDescription() const;
    QVector<double> supportedGains() const;

    /// @param db tuner gain, negative = automatic
    void setGain(double db);

    void startScan(const ScanParams &params);
    void abortScan();
    bool isScanning() const { return scanning_; }

    /// @brief start playback or retune if already playing
    void play(double freqHz);
    void stop();
    bool isPlaying() const { return playing_; }
    /// @brief false if the USB stream died (dongle unplugged etc.) while playing
    bool isStreamAlive() const;
    double frequency() const { return freqHz_; }

    void setBandwidth(double hz);
    void setVolume(double volume);

signals:
    void scanProgress(int percent);
    void scanFinished(const ScanResult &result, bool aborted);
    void liveSpectrum(const QVector<float> &db, double startHz, double binHz);
    void levelChanged(float dbfs);
    void errorOccurred(const QString &message);
    void playbackChanged(bool playing, double freqHz);

private:
    void onSamples(const sdr::IqSample *samples, size_t count);
    void updateSpectrum();

    std::unique_ptr<sdr::RtlSdrDriver> dev_;
    AudioOutput *audio_;

    std::thread scanThread_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> abortScan_{false};
    std::atomic<bool> playing_{false};
    std::atomic<double> freqHz_{0.0};

    // Settings passed to the stream thread
    std::atomic<double> bandwidth_{9000.0};
    std::atomic<bool> bandwidthChanged_{false};
    std::atomic<bool> resetDemod_{false};
    std::atomic<float> volume_{0.7f};

    // Stream thread only
    dsp::AmDemodulator demod_;
    std::vector<float> audioFloat_;
    std::vector<int16_t> audioPcm_;
    std::vector<dsp::cf> specInput_;
    std::vector<dsp::cf> specFrame_;
    std::vector<double> specPower_;
    std::vector<float> specWindow_;
    double specNorm_ = 1.0;
    size_t specFrames_ = 0;
    std::chrono::steady_clock::time_point lastSpectrum_;
};

#endif
