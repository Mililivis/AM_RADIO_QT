#include "AudioOutput.h"
#include "FileSource.h"
#include "MainWindow.h"
#include "RadioController.h"
#include "RtlSdrSource.h"
#include "dsp/AmDemodulator.h"
#include "dsp/Fft.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QPalette>
#include <QSocketNotifier>
#include <QStyleFactory>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace
{

int signalPipe[2] = {-1, -1};

/// @brief SIGTERM / SIGHUP (SSH closed) / SIGINT: wake up the event loop, quit cleanly and save settings
void onQuitSignal(int)
{
    char c = 1;
    [[maybe_unused]] ssize_t n = ::write(signalPipe[1], &c, 1);
}

void applyDarkPalette(QApplication &app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(37, 40, 44));
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    p.setColor(QPalette::Base, QColor(26, 28, 31));
    p.setColor(QPalette::AlternateBase, QColor(37, 40, 44));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    p.setColor(QPalette::Button, QColor(45, 48, 53));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
    p.setColor(QPalette::Highlight, QColor(42, 130, 218));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(45, 48, 53));
    p.setColor(QPalette::ToolTipText, QColor(220, 220, 220));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 110));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 110));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(110, 110, 110));
    app.setPalette(p);
}

} // namespace

int main(int argc, char **argv)
{
    // With --audio-stdout stdout carries raw PCM: keep a private copy of it for audio
    // and point fd 1 to stderr, so stray prints from libraries cannot corrupt the stream
    bool stdoutAudio = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--audio-stdout") == 0)
        {
            stdoutAudio = true;
        }
    }
    int audioFd = STDOUT_FILENO;
    if (stdoutAudio)
    {
        audioFd = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    std::signal(SIGPIPE, SIG_IGN);

    QApplication app(argc, argv);
    QApplication::setApplicationName("am_radio");
    QApplication::setOrganizationName("AM_RADIO_QT");
    applyDarkPalette(app);

    if (pipe(signalPipe) == 0)
    {
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
            char c;
            [[maybe_unused]] ssize_t n = ::read(signalPipe[0], &c, 1);
            QApplication::quit();
        });
        for (int sig : {SIGTERM, SIGHUP, SIGINT})
        {
            std::signal(sig, onQuitSignal);
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("AM radio for RTL-SDR: live spectrum and waterfall, panorama, "
                                     "IQ recording and rtl_tcp streaming");
    parser.addHelpOption();
    QCommandLineOption stdoutOption("audio-stdout",
                                    "Write audio as raw s16le mono 48 kHz to stdout (for piping over SSH)");
    QCommandLineOption deviceOption({"d", "device"}, "RTL-SDR device index", "index", "0");
    QCommandLineOption tuneOption({"t", "tune"}, "Start listening to this frequency at startup", "kHz");
    QCommandLineOption fileOption({"f", "file"}, "Replay an IQ recording (.sigmf-meta / .sigmf-data) instead of the dongle",
                                  "path");
    QCommandLineOption rtlTcpOption("rtltcp", "Start the rtl_tcp IQ server at startup", "port");
    QCommandLineOption screenshotOption("screenshot", "Save a screenshot of the window after 8 s and quit (testing)",
                                        "png");
    QCommandLineOption panoramaOption("panorama", "Start a continuous panorama of a range, e.g. 2300-26100", "kHz-kHz");
    parser.addOptions({stdoutOption, deviceOption, tuneOption, fileOption, rtlTcpOption, panoramaOption,
                       screenshotOption});
    parser.process(app);

    // ---------- Composition root: build the layers and wire them ----------
    core::AudioOutput audio(stdoutAudio ? core::AudioOutput::Backend::Stdout : core::AudioOutput::Backend::Pulse,
                            static_cast<unsigned>(dsp::AmDemodulator::audioRate), audioFd);
    std::string audioError;
    audio.start(audioError);

    std::unique_ptr<core::IqSource> source;
    if (parser.isSet(fileOption))
    {
        auto file = std::make_unique<core::FileSource>(parser.value(fileOption).toStdString());
        std::string error;
        if (!file->open(error))
        {
            QMessageBox::critical(nullptr, "AM Radio", QString::fromStdString(error));
            return 1;
        }
        source = std::move(file);
    }
    else
    {
        try
        {
            source = std::make_unique<core::RtlSdrSource>(static_cast<uint32_t>(parser.value(deviceOption).toUInt()));
        }
        catch (const std::exception &e)
        {
            QMessageBox::critical(nullptr, "AM Radio", QObject::tr("Не вдалося відкрити RTL-SDR:\n%1").arg(e.what()));
            return 1;
        }
    }
    std::fprintf(stderr, "AM radio: source %s, FFT %s\n", source->description().c_str(), dsp::Fft::backend());

    RadioController radio(std::move(source), &audio);
    MainWindow window(&radio, &audio, QString::fromStdString(audioError));
    window.show();

    if (parser.isSet(rtlTcpOption))
    {
        window.startNetwork(static_cast<quint16>(parser.value(rtlTcpOption).toUInt()));
    }
    if (parser.isSet(tuneOption))
    {
        double khz = parser.value(tuneOption).toDouble();
        QTimer::singleShot(0, &window, [&window, khz] { window.startListening(khz * 1e3); });
    }
    if (parser.isSet(panoramaOption))
    {
        const QStringList range = parser.value(panoramaOption).split('-');
        if (range.size() == 2)
        {
            double from = range[0].toDouble() * 1e3;
            double to = range[1].toDouble() * 1e3;
            QTimer::singleShot(0, &window, [&window, from, to] { window.startPanorama(from, to); });
        }
    }
    if (parser.isSet(screenshotOption))
    {
        const QString path = parser.value(screenshotOption);
        QTimer::singleShot(8000, &window, [&window, path] {
            window.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
