// Unit tests of the core and network modules (no Qt, no dongle needed)
#include "AsyncSink.h"
#include "FileSource.h"
#include "IqDispatcher.h"
#include "IqRecorder.h"
#include "RtlTcpServer.h"
#include "SpectrumAnalyzer.h"
#include "dsp/AmDemodulator.h"
#include "dsp/Rebin.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

namespace
{

constexpr double pi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const char *what, double value = NAN)
{
    std::printf("%s  %s", ok ? "[ OK ]" : "[FAIL]", what);
    if (!std::isnan(value))
    {
        std::printf("  (%.4g)", value);
    }
    std::printf("\n");
    if (!ok)
    {
        ++failures;
    }
}

double db(double x)
{
    return 10.0 * std::log10(x);
}

core::IqBlockPtr makeBlock(std::vector<core::cf> iq, double rate, double centre, uint64_t first)
{
    auto b = std::make_shared<core::IqBlock>();
    b->iq = std::move(iq);
    b->raw.resize(b->iq.size() * 2);
    for (size_t i = 0; i < b->iq.size(); ++i)
    {
        b->raw[2 * i] = static_cast<uint8_t>(std::clamp(b->iq[i].real() * 128.0f + 127.4f, 0.0f, 255.0f));
        b->raw[2 * i + 1] = static_cast<uint8_t>(std::clamp(b->iq[i].imag() * 128.0f + 127.4f, 0.0f, 255.0f));
    }
    b->sampleRate = rate;
    b->centerHz = centre;
    b->firstSample = first;
    b->time = std::chrono::system_clock::now();
    return b;
}

/// @brief run analyzer over a signal, return all lines
std::vector<core::SpectrumFramePtr> analyze(const std::vector<core::cf> &x, double rate, double lineRate)
{
    std::vector<core::SpectrumFramePtr> lines;
    core::SpectrumAnalyzer an([&](const core::SpectrumFramePtr &f) { lines.push_back(f); });
    an.setLineRate(lineRate);
    an.onStreamStart({rate, 0.0, "test"});
    const size_t block = 16384;
    for (size_t pos = 0; pos < x.size(); pos += block)
    {
        size_t n = std::min(block, x.size() - pos);
        an.onBlock(makeBlock(std::vector<core::cf>(x.begin() + pos, x.begin() + pos + n), rate, 0.0, pos));
    }
    an.onStreamStop();
    return lines;
}

double sum(const std::vector<float> &v)
{
    double s = 0.0;
    for (float x : v)
    {
        if (!std::isnan(x))
        {
            s += x;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------

void testRebinEnergy()
{
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<float> power(8192);
    for (auto &p : power)
    {
        p = u(rng);
    }
    const double start = 1000.0;
    const double bin = 293.0;
    std::vector<float> out;

    // Whole data range onto 700 pixels: every bit of energy must be kept
    dsp::rebinPower(power.data(), power.size(), start, bin, start, start + bin * power.size(), 700, out);
    check(std::fabs(sum(out) / sum(power) - 1.0) < 1e-5, "rebin: total energy preserved (8192 bins -> 700 px)",
          sum(out) / sum(power));

    // Zoomed window with fractional edges
    double from = start + 1234.5 * bin;
    double to = start + 3456.25 * bin;
    double expected = 0.0;
    for (size_t i = 0; i < power.size(); ++i)
    {
        double b0 = start + i * bin;
        double ov = std::min(b0 + bin, to) - std::max(b0, from);
        if (ov > 0)
        {
            expected += power[i] * ov / bin;
        }
    }
    dsp::rebinPower(power.data(), power.size(), start, bin, from, to, 333, out);
    check(std::fabs(sum(out) / expected - 1.0) < 1e-5, "rebin: energy of zoomed window preserved", sum(out) / expected);

    // A single narrow line must survive any compression
    std::vector<float> line(100000, 0.0f);
    line[54321] = 1.0f;
    dsp::rebinPower(line.data(), line.size(), 0.0, 10.0, 0.0, 1e6, 400, out);
    check(std::fabs(sum(out) - 1.0) < 1e-6, "rebin: single-bin carrier visible after 250:1 compression", sum(out));
}

void testAnalyzerEnergy()
{
    const double rate = 2.4e6;
    std::mt19937 rng(2);
    std::normal_distribution<float> nd(0.0f, 0.05f);   // complex noise power 2 * 0.05^2 = 0.005
    std::vector<core::cf> x(rate);                      // 1 s

    const double toneAmp = 0.3;                         // tone power 0.09
    const double toneHz = 300e3;
    for (size_t i = 0; i < x.size(); ++i)
    {
        x[i] = core::cf(nd(rng), nd(rng)) +
               core::cf(std::polar(toneAmp, 2 * pi * toneHz * static_cast<double>(i) / rate));
    }
    auto lines = analyze(x, rate, 10.0);
    check(!lines.empty(), "analyzer: produces lines", lines.size());

    const auto &f = *lines[lines.size() / 2];
    const double total = sum(f.power);
    check(std::fabs(db(total / (0.09 + 0.005))) < 0.2, "analyzer: sum of bins = signal power (dB error)",
          db(total / 0.095));

    // Tone energy within ±2 bins of the tone (whole window main lobe)
    size_t k = static_cast<size_t>((toneHz - f.startHz) / f.binHz);
    double tone = 0.0;
    for (size_t i = k - 4; i <= k + 4; ++i)
    {
        tone += f.power[i];
    }
    check(std::fabs(db(tone / 0.09)) < 0.2, "analyzer: full tone power in its main lobe (dB error)", db(tone / 0.09));
    check(f.binHz > 250 && f.binHz < 350, "analyzer: ~300 Hz resolution at 2.4 MS/s", f.binHz);
}

void testAnalyzerTimeCoverage()
{
    // Short burst at different positions: thanks to 75 % overlap it is never lost or attenuated
    const double rate = 960e3;
    double worst = 0.0;
    for (int trial = 0; trial < 12; ++trial)
    {
        std::vector<core::cf> x(static_cast<size_t>(rate * 0.5), core::cf(0, 0));
        const size_t start = 100000 + static_cast<size_t>(trial) * 377;
        const size_t len = 200;   // ~0.2 ms burst, much shorter than the FFT (4096 = 4.3 ms)
        for (size_t i = start; i < start + len; ++i)
        {
            x[i] = core::cf(std::polar(1.0, 2 * pi * 100e3 * static_cast<double>(i) / rate));
        }
        const double burstEnergy = static_cast<double>(len);   // |x|^2 = 1 per sample

        auto lines = analyze(x, rate, 5.0);
        double measured = 0.0;
        for (const auto &l : lines)
        {
            // mean power * line duration = energy
            double lineSeconds = l->averages * (core::SpectrumAnalyzer::fftSizeFor(rate) / 4) / rate;
            measured += sum(l->power) * lineSeconds * rate;
        }
        worst = std::max(worst, std::fabs(db(measured / burstEnergy)));
    }
    check(worst < 0.5, "analyzer: short bursts keep their energy at any time position (worst dB error)", worst);
}

double toneAmplitude(const std::vector<float> &a, double f, double fs)
{
    std::complex<double> acc = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        acc += static_cast<double>(a[i]) * std::polar(1.0, -2 * pi * f * static_cast<double>(i) / fs);
    }
    return 2.0 * std::abs(acc) / static_cast<double>(a.size());
}

void testAmDemodulator(double rate)
{
    // Wanted station at +150 kHz (1 kHz tone, m = 0.6), 20 dB stronger neighbour 9 kHz away (400 Hz tone)
    const double station = std::min(150e3, rate * 0.3);
    dsp::AmDemodulator dm;
    dm.setBandwidth(8000);
    dm.configure(rate);
    dm.setOffset(station);

    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.0f, 0.001f);
    std::vector<float> audio;
    std::vector<float> all;
    const size_t block = 32768;
    size_t t = 0;
    size_t counted = 0;
    const size_t total = static_cast<size_t>(rate * 1.5);
    while (t < total)
    {
        std::vector<core::cf> x(block);
        for (size_t i = 0; i < block; ++i, ++t)
        {
            double s = static_cast<double>(t) / rate;
            x[i] = core::cf(nd(rng), nd(rng))
                   + core::cf(std::polar(0.01 * (1 + 0.6 * std::cos(2 * pi * 1000 * s)), 2 * pi * station * s))
                   + core::cf(std::polar(0.1 * (1 + 0.6 * std::cos(2 * pi * 400 * s)), 2 * pi * (station + 9e3) * s));
        }
        dm.process(x.data(), x.size(), audio);
        if (t > rate * 0.5)
        {
            all.insert(all.end(), audio.begin(), audio.end());
            counted += block;
        }
    }

    char name[128];
    const double expectedSamples = 48000.0 * static_cast<double>(counted) / rate;
    std::snprintf(name, sizeof(name), "AM @ %.3f MS/s: audio rate 48 kHz (samples / expected)", rate / 1e6);
    check(std::fabs(all.size() / expectedSamples - 1.0) < 0.002, name, all.size() / expectedSamples);
    std::snprintf(name, sizeof(name), "AM @ %.3f MS/s: 1 kHz tone amplitude 0.6", rate / 1e6);
    double a1 = toneAmplitude(all, 1000, 48000);
    check(std::fabs(a1 - 0.6) < 0.05, name, a1);
    std::snprintf(name, sizeof(name), "AM @ %.3f MS/s: neighbour (+20 dB) rejected", rate / 1e6);
    double a4 = toneAmplitude(all, 400, 48000);
    check(a4 < 0.01, name, a4);
}

class SlowSink : public core::IqSink
{
public:
    std::atomic<uint64_t> received{0};
    void onBlock(const core::IqBlockPtr &b) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        received += b->size();
    }
};

void testAsyncSinkDrops()
{
    auto slow = std::make_shared<SlowSink>();
    auto async = std::make_shared<core::AsyncSink>(slow, 0.2, "slow");
    core::IqDispatcher dispatcher;
    dispatcher.addSink(async);
    dispatcher.streamStarted({1e6, 0.0, "t"});
    const size_t n = 100000;   // 0.1 s per block
    uint64_t pushed = 0;
    for (int i = 0; i < 30; ++i)
    {
        dispatcher.dispatch(makeBlock(std::vector<core::cf>(n), 1e6, 0.0, pushed));
        pushed += n;
    }
    dispatcher.streamStopped();
    async->flush();
    check(slow->received + async->droppedSamples() == pushed, "async sink: received + dropped = produced");
    check(async->droppedSamples() > 0, "async sink: overload drops instead of blocking", async->droppedSamples());
}

void testRecorder()
{
    const auto dir = std::filesystem::temp_directory_path() / "am_radio_rec_test";
    std::filesystem::remove_all(dir);

    {
        core::IqRecorder rec(dir.string());
        rec.onStreamStart({1.2e6, 1e6, "RTL-SDR test"});
        rec.onBlock(makeBlock(std::vector<core::cf>(1000), 1.2e6, 1e6, 0));
        rec.onBlock(makeBlock(std::vector<core::cf>(1000), 1.2e6, 1e6, 1000));
        rec.onBlock(makeBlock(std::vector<core::cf>(1000), 1.2e6, 1e6, 2500));   // 500 lost
        rec.onBlock(makeBlock(std::vector<core::cf>(1000), 1.2e6, 1.5e6, 3500));  // retuned
        rec.onStreamStop();
        auto st = rec.status();
        check(st.samples == 4000 && st.lostSamples == 500, "recorder: samples written / losses counted",
              static_cast<double>(st.samples));
    }

    std::string data;
    std::string meta;
    for (auto &e : std::filesystem::directory_iterator(dir))
    {
        if (e.path().extension() == ".sigmf-data")
        {
            data = e.path().string();
        }
        if (e.path().extension() == ".sigmf-meta")
        {
            meta = e.path().string();
        }
    }
    check(!data.empty() && std::filesystem::file_size(data) == 8000, "recorder: .sigmf-data has 4000 cu8 samples");
    std::ifstream in(meta);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    check(json.find("\"core:datatype\": \"cu8\"") != std::string::npos
              && json.find("\"core:sample_rate\": 1200000") != std::string::npos,
          "recorder: meta has datatype and sample rate");
    check(json.find("\"core:frequency\": 1500000") != std::string::npos
              && json.find("\"core:sample_start\": 3000") != std::string::npos,
          "recorder: retune recorded as new capture at sample 3000");
    check(json.find("500 samples missing") != std::string::npos, "recorder: data loss annotated");

    core::FileSource replay(meta);
    std::string error;
    check(replay.open(error) && replay.sampleRate() == 1.2e6 && replay.centerFrequency() == 1e6,
          "replay: SigMF metadata read back (rate, frequency)");
    std::filesystem::remove_all(dir);
}

void testRtlTcpServer()
{
    net::RtlTcpServer::Config cfg;
    cfg.bindAddress = "127.0.0.1";
    cfg.port = 23456;
    net::RtlTcpServer server(cfg);
    std::atomic<uint32_t> lastFreq{0};
    server.setCommandHandler([&](const std::string &, const net::RtlTcpServer::Command &c) {
        if (c.code == 1)
        {
            lastFreq = c.param;
        }
    });
    std::string error;
    check(server.start(error), "rtl_tcp: server starts");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    check(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "rtl_tcp: client connects");

    uint8_t header[12];
    size_t got = 0;
    while (got < 12)
    {
        ssize_t n = recv(fd, header + got, 12 - got, 0);
        if (n <= 0)
        {
            break;
        }
        got += static_cast<size_t>(n);
    }
    check(got == 12 && std::memcmp(header, "RTL0", 4) == 0 && header[7] == 6, "rtl_tcp: RTL0 header, tuner R828D");

    // Wait until the server registered the client, then stream 1 MB
    for (int i = 0; i < 50 && server.clients().empty(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<uint8_t> data(1 << 20);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<uint8_t>(i * 7);
    }
    for (size_t off = 0; off < data.size(); off += 65536)
    {
        server.send(data.data() + off, 65536);
    }
    std::vector<uint8_t> rx;
    rx.reserve(data.size());
    uint8_t buf[65536];
    while (rx.size() < data.size())
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        rx.insert(rx.end(), buf, buf + n);
    }
    check(rx == data, "rtl_tcp: 1 MB of IQ received bit-exact");

    uint8_t cmd[5] = {1, 0x00, 0x92, 0xB9, 0x8C};   // set frequency 9 615 756 Hz
    send(fd, cmd, 5, 0);
    for (int i = 0; i < 50 && lastFreq == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(lastFreq == 0x0092B98C, "rtl_tcp: client command decoded", lastFreq);

    close(fd);
    server.stop();
}

} // namespace

int main()
{
    testRebinEnergy();
    testAnalyzerEnergy();
    testAnalyzerTimeCoverage();
    for (double rate : {288e3, 960e3, 1.152e6, 2.048e6, 2.4e6})
    {
        testAmDemodulator(rate);
    }
    testAsyncSinkDrops();
    testRecorder();
    testRtlTcpServer();

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
