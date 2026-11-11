// Integration test with a real dongle (no GUI): live capture + receiver + recording + rtl_tcp client,
// then a panorama sweep and replay of the recording through the same pipeline.
// Usage: live_test [station kHz]
#include "AudioOutput.h"
#include "FileSource.h"
#include "RadioController.h"
#include "RtlSdrSource.h"
#include "dsp/AmDemodulator.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace
{
int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what.toUtf8().constData());
    if (!ok)
    {
        ++failures;
    }
}

void runFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

double cpuSeconds()
{
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const double stationHz = (argc > 1 ? std::atof(argv[1]) : 9615.0) * 1e3;
    const auto recDir = std::filesystem::temp_directory_path() / "am_radio_live_test";
    std::filesystem::remove_all(recDir);

    int devNull = open("/dev/null", O_WRONLY);
    core::AudioOutput audio(core::AudioOutput::Backend::Stdout, 48000, devNull);
    std::string audioError;
    audio.start(audioError);

    std::string recordedMeta;

    // ---------------------------------------------------------------- live
    {
        RadioController radio(std::make_unique<core::RtlSdrSource>(0), &audio);
        std::printf("source: %s\n", radio.sourceDescription().toUtf8().constData());

        int frames = 0;
        double lastPower = 0.0;
        size_t lastBins = 0;
        double frameStart = 0.0;
        double frameStop = 0.0;
        QObject::connect(&radio, &RadioController::spectrumFrame, [&](const core::SpectrumFramePtr &f) {
            ++frames;
            lastBins = f->power.size();
            frameStart = f->startHz;
            frameStop = f->stopHz();
            lastPower = 0.0;
            for (float p : f->power)
            {
                lastPower += p;
            }
        });

        radio.setGain(29.7);
        radio.setSpan(2.4e6);
        radio.setLineRate(10.0);
        radio.tune(stationHz);
        radio.setListening(true);
        check(radio.startLive(), "live: started");

        QString error;
        check(radio.startNetwork(23457, false, error), "rtl_tcp: server started " + error);
        check(radio.startRecording(QString::fromStdString(recDir.string()), error), "recording: started " + error);

        // rtl_tcp client on another thread, like SDR++ on another host would be
        std::atomic<uint64_t> netBytes{0};
        std::atomic<bool> netStop{false};
        std::thread client([&] {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(23457);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            {
                return;
            }
            timeval tv{0, 200000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            static uint8_t buf[1 << 16];
            while (!netStop)
            {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n > 0)
                {
                    netBytes += static_cast<uint64_t>(n);
                }
            }
            close(fd);
        });

        runFor(1000);   // settle
        const double cpu0 = cpuSeconds();
        const int frames0 = frames;
        const uint64_t net0 = netBytes;
        runFor(5000);
        const double cpu = (cpuSeconds() - cpu0) / 5.0 * 100.0;
        const double fps = (frames - frames0) / 5.0;
        const double netRate = (netBytes - net0) / 5.0;

        float level = radio.level();
        auto rec = radio.recordingStatus();
        netStop = true;
        client.join();
        radio.stopRecording();
        auto recFinal = radio.recordingStatus();
        radio.stopNetwork();

        std::printf("  window %.3f-%.3f MHz, %zu bins, line power %.1f dBFS, station level %.1f dBFS\n",
                    frameStart / 1e6, frameStop / 1e6, lastBins, 10 * std::log10(lastPower), level);
        std::printf("  spectrum %.1f lines/s, rtl_tcp %.2f MB/s, CPU %.0f %% of one core (all threads)\n",
                    fps, netRate / 1e6, cpu);
        std::printf("  recorded %.1f s, %.1f MB, lost %llu samples -> %s\n", recFinal.seconds, recFinal.bytes / 1e6,
                    static_cast<unsigned long long>(recFinal.lostSamples), recFinal.dataPath.c_str());

        check(std::fabs(frameStop - frameStart - 2.4e6) < 1e3, "live: spectrum covers the whole 2.4 MHz window");
        check(fps > 8.0 && fps < 12.0, QString("live: ~10 spectrum lines/s (%1)").arg(fps));
        check(level > -100.0f, QString("receiver: station inside window, level %1 dBFS").arg(level));
        check(std::fabs(netRate - 4.8e6) < 0.3e6, QString("rtl_tcp: client gets the full 4.8 MB/s (%1)").arg(netRate / 1e6));
        check(rec.writing && recFinal.seconds > 5.5, "recording: wrote the whole time");
        check(recFinal.lostSamples == 0, "recording: no lost samples");
        check(audio.isRunning(), "audio: running");
        recordedMeta = recFinal.dataPath.substr(0, recFinal.dataPath.size() - 4) + "meta";

        // ------------------------------------------------------------ sweep
        core::ScanResult sweep;
        bool sweepDone = false;
        QObject::connect(&radio, &RadioController::sweepFinished, [&](const core::ScanResult &r, bool) {
            sweep = r;
            sweepDone = true;
        });
        core::ScanParams p;
        p.startHz = 5.9e6;
        p.stopHz = 12.1e6;   // 49 m .. 25 m: wider than one capture
        p.gridHz = 5e3;
        const auto t0 = std::chrono::steady_clock::now();
        radio.startSweep(p, false);
        while (!sweepDone)
        {
            runFor(50);
        }
        runFor(500);   // live resumes
        const double sweepSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t holes = 0;
        for (float v : sweep.power)
        {
            holes += std::isnan(v) ? 1 : 0;
        }
        std::printf("  panorama 5.9-12.1 MHz: %.1f s, %zu bins, %zu stations\n", sweepSeconds, sweep.power.size(),
                    sweep.stations.size());
        check(holes == 0, "panorama: no holes in the stitched spectrum");
        check(radio.mode() == RadioController::Mode::Live, "panorama: live view resumed afterwards");
        radio.stop();
    }

    // ---------------------------------------------------------------- replay
    {
        auto file = std::make_unique<core::FileSource>(recordedMeta);
        std::string error;
        if (!file->open(error))
        {
            check(false, "replay: recording opens " + QString::fromStdString(error));
            return 1;
        }
        const double rate = file->sampleRate();
        const double centre = file->centerFrequency();
        RadioController radio(std::move(file), &audio);
        int frames = 0;
        double frameCentre = 0.0;
        QObject::connect(&radio, &RadioController::spectrumFrame, [&](const core::SpectrumFramePtr &f) {
            ++frames;
            frameCentre = f->centerHz;
        });
        radio.tune(stationHz);
        radio.setListening(true);
        radio.startLive();
        runFor(2000);
        std::printf("  replay: %.3f MS/s at %.3f MHz, %d lines in 2 s, level %.1f dBFS\n", rate / 1e6, centre / 1e6,
                    frames, radio.level());
        check(frames >= 15 && frameCentre == centre, "replay: recording plays through the same pipeline");
        check(radio.level() > -100.0f, "replay: station demodulated from the file");
        radio.stop();
    }

    std::filesystem::remove_all(recDir);
    audio.stop();
    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
