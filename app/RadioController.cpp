#include "RadioController.h"

#include "AudioOutput.h"
#include "NetworkIqSink.h"
#include "RtlSdrSource.h"

#include <QMetaObject>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double receiverQueueSeconds = 0.5;
constexpr double analyzerQueueSeconds = 0.5;
constexpr double recorderQueueSeconds = 4.0;   // absorbs SD card write stalls
constexpr int maxFramesInFlight = 3;           // GUI (over X11) slower than analyzer -> skip lines
constexpr double vfoMargin = 0.45;             // usable part of the capture around the centre
constexpr double dcAvoidHz = 10e3;             // do not demodulate on top of the DC spike
constexpr unsigned continuousSweepAverages = 24;
} // namespace

RadioController::RadioController(std::unique_ptr<core::IqSource> source, core::AudioOutput *audio, QObject *parent)
    : QObject(parent), source_(std::move(source)), audio_(audio)
{
    qRegisterMetaType<core::SpectrumFramePtr>("core::SpectrumFramePtr");
    qRegisterMetaType<core::ScanResult>("core::ScanResult");

    rtl_ = dynamic_cast<core::RtlSdrSource *>(source_.get());

    analyzer_ = std::make_shared<core::SpectrumAnalyzer>([this](const core::SpectrumFramePtr &f) { deliverFrame(f); });
    analyzerSink_ = std::make_shared<core::AsyncSink>(analyzer_, analyzerQueueSeconds, "analyzer");
    receiver_ = std::make_shared<core::AmReceiver>(audio_);
    receiverSink_ = std::make_shared<core::AsyncSink>(receiver_, receiverQueueSeconds, "receiver");
    dispatcher_.addSink(analyzerSink_);
    dispatcher_.addSink(receiverSink_);

    if (!isTunable())
    {
        vfo_ = source_->centerFrequency() + source_->sampleRate() / 4.0;
    }
    receiver_->setFrequency(vfo_);
}

RadioController::~RadioController()
{
    abortSweep_ = true;
    if (sweepThread_.joinable())
    {
        sweepThread_.join();
    }
    stopNetwork();
    stopRecording();
    stopSource();
    dispatcher_.removeSink(receiverSink_);
    dispatcher_.removeSink(analyzerSink_);
}

QString RadioController::sourceDescription() const
{
    return QString::fromStdString(source_->description());
}

bool RadioController::isTunable() const
{
    return source_->isTunable();
}

QVector<double> RadioController::supportedGains() const
{
    QVector<double> gains;
    if (rtl_)
    {
        for (double g : rtl_->supportedGains())
        {
            gains.push_back(g);
        }
    }
    return gains;
}

QVector<double> RadioController::supportedSpans()
{
    // Multiples of 48 kHz: integer decimation to audio rate. 225..300 kHz and 0.9..3.2 MHz are valid for RTL2832
    return {288e3, 960e3, 1.152e6, 1.44e6, 1.92e6, 2.4e6};
}

double RadioController::span() const
{
    return source_->sampleRate();
}

double RadioController::center() const
{
    return source_->centerFrequency();
}

void RadioController::setMode(Mode mode)
{
    if (mode_ != mode)
    {
        mode_ = mode;
        emit modeChanged(mode);
    }
}

void RadioController::emitTuning()
{
    emit tuningChanged(center(), span(), vfo_);
}

// ---------------------------------------------------------------------------
// Live
// ---------------------------------------------------------------------------

bool RadioController::startSource()
{
    dispatcher_.streamStarted(source_->streamInfo());
    std::string error;
    if (!source_->start([this](const core::IqBlockPtr &b) { dispatcher_.dispatch(b); }, error))
    {
        dispatcher_.streamStopped();
        emit errorOccurred(QString::fromStdString(error));
        return false;
    }
    return true;
}

void RadioController::stopSource()
{
    if (source_->isRunning())
    {
        source_->stop();
    }
    dispatcher_.streamStopped();
}

bool RadioController::startLive()
{
    if (mode_ == Mode::Live)
    {
        return true;
    }
    if (mode_ == Mode::Sweep)
    {
        resumeLive_ = true;   // switch over as soon as the sweep thread has stopped
        abortSweep_ = true;
        return true;
    }
    if (!startSource())
    {
        return false;
    }
    setMode(Mode::Live);
    emitTuning();
    return true;
}

void RadioController::stop()
{
    if (mode_ == Mode::Sweep)
    {
        resumeLive_ = false;
        abortSweep_ = true;
        return;
    }
    if (mode_ == Mode::Live)
    {
        stopSource();
        setMode(Mode::Idle);
    }
}

void RadioController::setSpan(double hz)
{
    if (!isTunable() || hz == span() || mode_ == Mode::Sweep)
    {
        return;
    }
    try
    {
        const bool live = mode_ == Mode::Live;
        if (live)
        {
            stopSource();
        }
        source_->setSampleRate(hz);
        if (live)
        {
            startSource();
        }
    }
    catch (const std::exception &e)
    {
        emit errorOccurred(e.what());
    }

    // Keep the station inside the new window
    if (std::fabs(vfo_ - center()) > vfoMargin * span())
    {
        tune(vfo_);
        return;
    }
    emitTuning();
}

void RadioController::setCenter(double hz)
{
    if (!isTunable() || mode_ == Mode::Sweep)
    {
        return;
    }
    try
    {
        source_->setCenterFrequency(hz);
    }
    catch (const std::exception &e)
    {
        emit errorOccurred(e.what());
        return;
    }
    // Station that fell out of the window is pulled to its nearest edge
    const double half = vfoMargin * span();
    vfo_ = std::clamp(vfo_, hz - half, hz + half);
    receiver_->setFrequency(vfo_);
    emitTuning();
}

void RadioController::tune(double hz)
{
    const double offset = hz - center();
    const bool outside = std::fabs(offset) > vfoMargin * span() || std::fabs(offset) < dcAvoidHz;

    if (outside)
    {
        if (!isTunable())
        {
            emit errorOccurred(tr("%1 кГц поза записаною смугою").arg(hz / 1e3, 0, 'f', 1));
            return;
        }
        if (mode_ != Mode::Sweep)
        {
            // Station a quarter of the window left of centre: far from DC and from the edges
            vfo_ = hz;
            receiver_->setFrequency(vfo_);
            setCenter(hz + span() / 4.0);
            return;
        }
    }
    vfo_ = hz;
    receiver_->setFrequency(vfo_);
    emitTuning();
}

void RadioController::setListening(bool on)
{
    listening_ = on;
    receiver_->setMuted(!on);
    if (audio_)
    {
        audio_->clear();
    }
}

void RadioController::setBandwidth(double hz)
{
    receiver_->setBandwidth(hz);
}

void RadioController::setVolume(double volume)
{
    receiver_->setVolume(volume);
}

void RadioController::setGain(double db)
{
    if (!rtl_ || mode_ == Mode::Sweep)
    {
        return;
    }
    try
    {
        rtl_->setGain(db);
    }
    catch (const std::exception &e)
    {
        emit errorOccurred(e.what());
    }
}

void RadioController::setLineRate(double linesPerSecond)
{
    analyzer_->setLineRate(linesPerSecond);
}

float RadioController::level() const
{
    return receiver_->levelDb();
}

void RadioController::deliverFrame(const core::SpectrumFramePtr &frame)
{
    // Called from the analyzer thread. If the GUI has not drawn the previous lines yet, skip this one
    if (framesInFlight_ >= maxFramesInFlight)
    {
        return;
    }
    ++framesInFlight_;
    QMetaObject::invokeMethod(this, [this, frame] {
        --framesInFlight_;
        emit spectrumFrame(frame);
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

void RadioController::startSweep(const core::ScanParams &params, bool continuous)
{
    if (!rtl_ || mode_ == Mode::Sweep)
    {
        return;
    }
    if (sweepThread_.joinable())
    {
        sweepThread_.join();
    }

    resumeLive_ = mode_ == Mode::Live;
    stopSource();
    abortSweep_ = false;
    continuous_ = continuous;
    setMode(Mode::Sweep);

    core::ScanParams p = params;
    if (continuous)
    {
        p.averages = std::min(p.averages, continuousSweepAverages);
    }

    sweepThread_ = std::thread([this, p] {
        do
        {
            core::ScanResult result;
            bool failed = false;
            try
            {
                result = core::runScan(rtl_->driver(), p, abortSweep_, [this](int pct) {
                    QMetaObject::invokeMethod(this, [this, pct] { emit sweepProgress(pct); }, Qt::QueuedConnection);
                });
            }
            catch (const std::exception &e)
            {
                failed = true;
                QString msg = e.what();
                QMetaObject::invokeMethod(this, [this, msg] { emit errorOccurred(msg); }, Qt::QueuedConnection);
            }

            const bool aborted = failed || abortSweep_;
            if (!result.power.empty())
            {
                auto frame = std::make_shared<core::SpectrumFrame>();
                frame->startHz = result.startHz;
                frame->binHz = result.binHz;
                frame->power = result.power;
                frame->centerHz = (p.startHz + p.stopHz) / 2.0;
                frame->sweep = true;
                frame->time = std::chrono::system_clock::now();
                QMetaObject::invokeMethod(this, [this, frame, result, aborted] {
                    emit spectrumFrame(frame);
                    emit sweepFinished(result, aborted);
                }, Qt::QueuedConnection);
            }
            if (failed)
            {
                break;
            }
        } while (continuous_ && !abortSweep_);

        QMetaObject::invokeMethod(this, [this] { finishSweep(); }, Qt::QueuedConnection);
    });
}

void RadioController::finishSweep()
{
    if (sweepThread_.joinable())
    {
        sweepThread_.join();
    }
    try
    {
        rtl_->restoreSettings();
    }
    catch (const std::exception &e)
    {
        emit errorOccurred(e.what());
    }
    setMode(Mode::Idle);
    if (resumeLive_)
    {
        resumeLive_ = false;
        startLive();
    }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

bool RadioController::startRecording(const QString &directory, QString &error)
{
    if (recorder_)
    {
        return true;
    }
    if (directory.isEmpty())
    {
        error = tr("не вказано папку для запису");
        return false;
    }
    recorder_ = std::make_shared<core::IqRecorder>(directory.toStdString());
    recorderSink_ = std::make_shared<core::AsyncSink>(recorder_, recorderQueueSeconds, "recorder");
    dispatcher_.addSink(recorderSink_);
    return true;
}

void RadioController::stopRecording()
{
    if (!recorder_)
    {
        return;
    }
    dispatcher_.removeSink(recorderSink_);
    recorderSink_->flush();
    lastRecording_ = recorder_->status();
    lastRecording_.lostSamples += recorderSink_->droppedSamples();
    recorderSink_.reset();
    recorder_.reset();
}

core::RecorderStatus RadioController::recordingStatus() const
{
    if (!recorder_)
    {
        return lastRecording_;
    }
    core::RecorderStatus st = recorder_->status();
    // Blocks dropped before reaching the recorder are already counted as gaps in the file,
    // this adds the ones still in flight
    st.lostSamples = std::max<uint64_t>(st.lostSamples, recorderSink_->droppedSamples());
    return st;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

bool RadioController::startNetwork(quint16 port, bool allowControl, QString &error)
{
    if (server_)
    {
        allowControl_ = allowControl;
        return true;
    }

    net::RtlTcpServer::Config cfg;
    cfg.port = port;
    cfg.gainCount = static_cast<uint32_t>(std::max(1, static_cast<int>(supportedGains().size())));
    auto server = std::make_shared<net::RtlTcpServer>(cfg);

    server->setCommandHandler([this](const std::string &, const net::RtlTcpServer::Command &c) {
        quint8 code = c.code;
        quint32 param = c.param;
        QMetaObject::invokeMethod(this, [this, code, param] { onRemoteCommand(code, param); }, Qt::QueuedConnection);
    });

    std::string err;
    if (!server->start(err))
    {
        error = QString::fromStdString(err);
        return false;
    }

    server_ = server;
    allowControl_ = allowControl;
    networkSink_ = std::make_shared<net::NetworkIqSink>(server_);
    dispatcher_.addSink(networkSink_);
    return true;
}

void RadioController::stopNetwork()
{
    if (!server_)
    {
        return;
    }
    dispatcher_.removeSink(networkSink_);
    server_->stop();
    networkSink_.reset();
    server_.reset();
}

RadioController::NetworkStatus RadioController::networkStatus() const
{
    NetworkStatus st;
    if (server_)
    {
        st.running = true;
        st.port = server_->config().port;
        st.allowControl = allowControl_;
        st.clients = server_->clients();
    }
    return st;
}

void RadioController::onRemoteCommand(quint8 code, quint32 param)
{
    if (!allowControl_ || !isTunable())
    {
        return;
    }

    switch (code)
    {
    case 0x01:   // centre frequency
        setCenter(static_cast<double>(param));
        break;
    case 0x02:   // sample rate -> nearest supported window width
    {
        const auto spans = supportedSpans();
        double best = spans.front();
        for (double s : spans)
        {
            if (std::fabs(s - param) < std::fabs(best - param))
            {
                best = s;
            }
        }
        setSpan(best);
        break;
    }
    case 0x03:   // gain mode: 0 = auto
        if (param == 0)
        {
            setGain(-1.0);
            emit gainChanged(-1.0);
        }
        break;
    case 0x04:   // gain in tenths of dB
        setGain(param / 10.0);
        emit gainChanged(param / 10.0);
        break;
    default:
        break;   // other rtl_tcp commands (ppm, AGC, direct sampling...) are not forwarded
    }
}
