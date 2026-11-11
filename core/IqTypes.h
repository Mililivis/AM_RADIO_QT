#ifndef CORE_IQ_TYPES_H
#define CORE_IQ_TYPES_H

#include <chrono>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core
{

using cf = std::complex<float>;

/// @brief parameters of a continuous IQ stream
struct StreamInfo
{
    double sampleRate = 0.0;
    double centerHz = 0.0;
    std::string description;   // e.g. "RTL-SDR Blog V4, R828D" (for metadata)
};

/// @brief one chunk of the IQ stream, immutable after creation and shared by all sinks
struct IqBlock
{
    uint64_t sequence = 0;        // block counter since stream start
    uint64_t firstSample = 0;     // index of the first sample since stream start (gaps are visible as jumps)
    double sampleRate = 0.0;
    double centerHz = 0.0;
    std::chrono::system_clock::time_point time;   // wall clock time of the first sample

    std::vector<uint8_t> raw;     // native RTL2832 format: interleaved unsigned 8-bit I, Q (cu8)
    std::vector<cf> iq;           // the same samples as complex float in [-1, 1]

    size_t size() const { return iq.size(); }
};

using IqBlockPtr = std::shared_ptr<const IqBlock>;

} // namespace core

#endif
