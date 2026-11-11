#ifndef CORE_IQ_SOURCE_H
#define CORE_IQ_SOURCE_H

#include "IqTypes.h"

#include <functional>
#include <string>

namespace core
{

/// @brief producer of the IQ stream (RTL-SDR dongle, recorded file...)
class IqSource
{
public:
    using BlockCallback = std::function<void(const IqBlockPtr &)>;

    virtual ~IqSource() = default;

    virtual std::string description() const = 0;

    /// @brief start producing blocks, callback is called from the source's own thread
    virtual bool start(BlockCallback callback, std::string &error) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    /// @brief false for sources that cannot be retuned (files)
    virtual bool isTunable() const = 0;
    virtual void setCenterFrequency(double hz) = 0;
    virtual double centerFrequency() const = 0;

    /// @brief may restart the stream internally
    virtual void setSampleRate(double hz) = 0;
    virtual double sampleRate() const = 0;

    StreamInfo streamInfo() const { return {sampleRate(), centerFrequency(), description()}; }
};

} // namespace core

#endif
