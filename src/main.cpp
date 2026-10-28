#include "AudioOutput.h"
#include "MainWindow.h"
#include "RadioEngine.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QPalette>
#include <QSocketNotifier>
#include <QStyleFactory>
#include <QTimer>

#include <csignal>
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
    parser.setApplicationDescription("AM radio for RTL-SDR with band scanner");
    parser.addHelpOption();
    QCommandLineOption stdoutOption("audio-stdout",
                                    "Write audio as raw s16le mono 48 kHz to stdout (for piping over SSH)");
    QCommandLineOption deviceOption({"d", "device"}, "RTL-SDR device index", "index", "0");
    parser.addOption(stdoutOption);
    QCommandLineOption tuneOption({"t", "tune"}, "Start listening to this frequency at startup", "kHz");
    parser.addOption(deviceOption);
    parser.addOption(tuneOption);
    parser.process(app);

    AudioOutput audio(stdoutAudio ? AudioOutput::Backend::Stdout : AudioOutput::Backend::Pulse,
                      static_cast<unsigned>(dsp::AmDemodulator::audioRate), audioFd);
    std::string audioError;
    audio.start(audioError);

    RadioEngine engine(&audio);
    QString error;
    if (!engine.open(parser.value(deviceOption).toInt(), error))
    {
        QMessageBox::critical(nullptr, "AM Radio", QObject::tr("Не вдалося відкрити RTL-SDR:\n%1").arg(error));
        return 1;
    }

    MainWindow window(&engine, &audio, QString::fromStdString(audioError));
    window.show();
    if (parser.isSet(tuneOption))
    {
        double khz = parser.value(tuneOption).toDouble();
        QTimer::singleShot(0, &window, [&window, khz] { window.startPlayback(khz * 1e3); });
    }
    return app.exec();
}
