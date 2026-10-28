#include "RtlSdrDriver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> stopRequested{false};

void onSignal(int)
{
    stopRequested = true;
}

void printUsage(const char *prog)
{
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  -l            list devices and exit\n"
        << "  -d <index>    device index (default 0)\n"
        << "  -S <serial>   device serial (instead of -d)\n"
        << "  -f <Hz>       center frequency (default 100e6), e.g. 100e6, 7.1e6\n"
        << "  -s <Hz>       sample rate (default 2.048e6)\n"
        << "  -g <dB|auto>  tuner gain (default auto)\n"
        << "  -p <ppm>      frequency correction\n"
        << "  -T            enable bias tee\n"
        << "  -n <count>    number of I/Q samples to read (default 2^20)\n"
        << "  -o <file>     write raw u8 I/Q to file (rtl_sdr compatible, '-' = stdout)\n"
        << "  -h            this help\n";
}

uint32_t parseHz(const char *s)
{
    return static_cast<uint32_t>(std::llround(std::strtod(s, nullptr)));
}

void printStats(const std::vector<sdr::IqSample> &iq)
{
    if (iq.empty())
    {
        std::cerr << "No samples\n";
        return;
    }

    double power = 0.0;
    sdr::IqSample dc(0.0f, 0.0f);
    size_t clipped = 0;
    for (const auto &s : iq)
    {
        power += std::norm(s);
        dc += s;
        if (std::fabs(s.real()) > 0.99f || std::fabs(s.imag()) > 0.99f)
        {
            ++clipped;
        }
    }
    power /= static_cast<double>(iq.size());
    dc /= static_cast<float>(iq.size());

    std::cerr << std::fixed << std::setprecision(2)
              << "Samples:     " << iq.size() << '\n'
              << "Mean power:  " << 10.0 * std::log10(power + 1e-12) << " dBFS\n"
              << "DC offset:   " << dc.real() << " + j" << dc.imag() << '\n'
              << "Clipping:    " << 100.0 * clipped / iq.size() << " %"
              << (clipped * 100 > iq.size() ? "  <- reduce gain" : "") << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    int deviceIndex = 0;
    std::string serial;
    uint32_t frequency = 100'000'000;
    uint32_t sampleRate = sdr::RtlSdrDriver::defaultSampleRate;
    std::string gainArg = "auto";
    int ppm = 0;
    bool biasTee = false;
    size_t sampleCount = 1 << 20;
    std::string outFile;
    bool listOnly = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << arg << '\n';
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-l")      listOnly = true;
        else if (arg == "-d") deviceIndex = std::atoi(next());
        else if (arg == "-S") serial = next();
        else if (arg == "-f") frequency = parseHz(next());
        else if (arg == "-s") sampleRate = parseHz(next());
        else if (arg == "-g") gainArg = next();
        else if (arg == "-p") ppm = std::atoi(next());
        else if (arg == "-T") biasTee = true;
        else if (arg == "-n") sampleCount = static_cast<size_t>(std::strtod(next(), nullptr));
        else if (arg == "-o") outFile = next();
        else
        {
            printUsage(argv[0]);
            return arg == "-h" ? 0 : 1;
        }
    }

    auto devices = sdr::RtlSdrDriver::listDevices();
    std::cerr << "Found " << devices.size() << " device(s)\n";
    for (const auto &d : devices)
    {
        std::cerr << "  #" << d.index << ": " << d.manufacturer << ", " << d.product
                  << ", SN: " << d.serial << '\n';
    }
    if (listOnly)
    {
        return 0;
    }

    try
    {
        uint32_t index = serial.empty() ? static_cast<uint32_t>(deviceIndex)
                                        : sdr::RtlSdrDriver::indexBySerial(serial);
        sdr::RtlSdrDriver dev(index);

        std::cerr << "Opened #" << index << ", tuner: " << dev.tunerName() << '\n';

        std::cerr << "Supported gains (dB):";
        for (double g : dev.supportedGains())
        {
            std::cerr << ' ' << g;
        }
        std::cerr << '\n';

        dev.setSampleRate(sampleRate);
        dev.setCenterFrequency(frequency);
        dev.setFrequencyCorrection(ppm);
        dev.setBiasTee(biasTee);

        if (gainArg == "auto")
        {
            dev.setGainMode(sdr::GainMode::Auto);
            std::cerr << "Gain: auto\n";
        }
        else
        {
            double applied = dev.setGain(std::strtod(gainArg.c_str(), nullptr));
            std::cerr << "Gain: " << applied << " dB\n";
        }

        std::cerr << "Frequency: " << dev.centerFrequency() << " Hz, sample rate: "
                  << dev.sampleRate() << " Hz\n";

        if (outFile.empty())
        {
            // Synchronous read + statistics: quick check that the dongle delivers data
            dev.resetBuffer();
            auto iq = dev.readIq(sampleCount);
            printStats(iq);
            return 0;
        }

        // Asynchronous streaming to file until sampleCount samples or Ctrl+C
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        std::ofstream file;
        std::ostream *out = &std::cout;
        if (outFile != "-")
        {
            file.open(outFile, std::ios::binary);
            if (!file)
            {
                std::cerr << "Cannot open " << outFile << '\n';
                return 1;
            }
            out = &file;
        }

        size_t bytesLeft = sampleCount * 2;
        std::atomic<bool> done{false};

        dev.startStreamingRaw([&](const uint8_t *data, size_t len) {
            if (done)
            {
                return;
            }
            size_t toWrite = std::min(len, bytesLeft);
            out->write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(toWrite));
            bytesLeft -= toWrite;
            if (bytesLeft == 0)
            {
                done = true;
            }
        });

        std::cerr << "Streaming... (Ctrl+C to stop)\n";
        while (!done && !stopRequested && dev.isStreaming())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        dev.stopStreaming();
        out->flush();

        std::cerr << "Written " << (sampleCount * 2 - bytesLeft) / 2 << " samples\n";
    }
    catch (const sdr::RtlSdrError &e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
