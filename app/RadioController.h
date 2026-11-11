#ifndef APP_RADIO_CONTROLLER_H
#define APP_RADIO_CONTROLLER_H

#include "AmReceiver.h"
#include "AsyncSink.h"
#include "IqDispatcher.h"
#include "IqRecorder.h"
#include "IqSource.h"
#include "RtlTcpServer.h"
#include "Scanner.h"
#include "SpectrumAnalyzer.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>
#include <thread>

namespace core
{
class AudioOutput;
class RtlSdrSource;
}
namespace net
{
class NetworkIqSink;
}

Q_DECLARE_METATYPE(core::SpectrumFramePtr)
Q_DECLARE_METATYPE(core::ScanResult)

/// @brief application layer: assembles the IQ pipeline and runs the modes.
///
///   source ──► IqDispatcher ──┬─► AsyncSink ─► SpectrumAnalyzer ─► spectrumFrame()  (spectrum + waterfall)
///                             ├─► AsyncSink ─► AmReceiver ─► AudioOutput
///                             ├─► AsyncSink ─► IqRecorder            (while recording)
///                             └─► NetworkIqSink ─► RtlTcpServer      (while streaming to the network)
///
/// Modes: Idle, Live (continuous capture of up to 2.4 MHz) and Sweep (panorama of any range
/// by retuning, no audio). All public methods are called from the GUI thread;
/// signals coming from worker threads are delivered as queued connections.
class RadioController : public QObject
{
    Q_OBJECT

public:
    enum class Mode
    {
        Idle,
        Live,
        Sweep
    };
    Q_ENUM(Mode)

    struct NetworkStatus
    {
        bool running = false;
        quint16 port = 0;
        bool allowControl = false;
        std::vector<net::RtlTcpServer::ClientInfo> clients;
    };

    RadioController(std::unique_ptr<core::IqSource> source, core::AudioOutput *audio, QObject *parent = nullptr);
    ~RadioController() override;

    QString sourceDescription() const;
    bool canSweep() const { return rtl_ != nullptr; }
    bool isTunable() const;
    QVector<double> supportedGains() const;
    /// @brief capture widths of the live mode (sample rates of the dongle), Hz
    static QVector<double> supportedSpans();

    Mode mode() const { return mode_; }
    double span() const;
    double center() const;
    double vfo() const { return vfo_; }

    // ---------- Live ----------
    bool startLive();
    /// @brief stop live capture or sweep
    void stop();
    void setSpan(double hz);
    void setCenter(double hz);
    /// @brief set the receiver frequency; recentres the capture if the station is outside of it
    void tune(double hz);
    void setListening(bool on);
    bool isListening() const { return listening_; }
    void setBandwidth(double hz);
    void setVolume(double volume);
    /// @param db negative = automatic
    void setGain(double db);
    void setLineRate(double linesPerSecond);
    float level() const;

    // ---------- Sweep (panorama, station search) ----------
    void startSweep(const core::ScanParams &params, bool continuous);
    bool isContinuousSweep() const { return mode_ == Mode::Sweep && continuous_; }

    // ---------- IQ recording ----------
    bool startRecording(const QString &directory, QString &error);
    void stopRecording();
    bool isRecording() const { return recorder_ != nullptr; }
    core::RecorderStatus recordingStatus() const;

    // ---------- IQ network streaming ----------
    bool startNetwork(quint16 port, bool allowControl, QString &error);
    void stopNetwork();
    NetworkStatus networkStatus() const;

signals:
    void modeChanged(RadioController::Mode mode);
    void spectrumFrame(const core::SpectrumFramePtr &frame);
    void sweepProgress(int percent);
    void sweepFinished(const core::ScanResult &result, bool aborted);
    void tuningChanged(double centerHz, double spanHz, double vfoHz);
    void gainChanged(double db);
    void errorOccurred(const QString &message);

private:
    bool startSource();
    void stopSource();
    void setMode(Mode mode);
    void finishSweep();
    void deliverFrame(const core::SpectrumFramePtr &frame);
    void onRemoteCommand(quint8 code, quint32 param);
    void emitTuning();

    std::unique_ptr<core::IqSource> source_;
    core::RtlSdrSource *rtl_ = nullptr;
    core::AudioOutput *audio_;
    core::IqDispatcher dispatcher_;

    std::shared_ptr<core::SpectrumAnalyzer> analyzer_;
    std::shared_ptr<core::AsyncSink> analyzerSink_;
    std::shared_ptr<core::AmReceiver> receiver_;
    std::shared_ptr<core::AsyncSink> receiverSink_;
    std::shared_ptr<core::IqRecorder> recorder_;
    std::shared_ptr<core::AsyncSink> recorderSink_;
    core::RecorderStatus lastRecording_;
    std::shared_ptr<net::RtlTcpServer> server_;
    std::shared_ptr<net::NetworkIqSink> networkSink_;
    bool allowControl_ = false;

    Mode mode_ = Mode::Idle;
    double vfo_ = 999e3;
    bool listening_ = false;
    std::atomic<int> framesInFlight_{0};

    std::thread sweepThread_;
    std::atomic<bool> abortSweep_{false};
    std::atomic<bool> continuous_{false};
    bool resumeLive_ = false;
};

#endif
