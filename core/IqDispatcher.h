#ifndef CORE_IQ_DISPATCHER_H
#define CORE_IQ_DISPATCHER_H

#include "IqSink.h"

#include <memory>
#include <mutex>
#include <vector>

namespace core
{

/// @brief fan-out of one IQ stream to any number of sinks.
/// Sinks can be attached and detached while streaming: a sink added mid-stream gets
/// onStreamStart immediately, a removed one gets onStreamStop.
class IqDispatcher
{
public:
    void addSink(const std::shared_ptr<IqSink> &sink);
    void removeSink(const std::shared_ptr<IqSink> &sink);

    void streamStarted(const StreamInfo &info);
    void dispatch(const IqBlockPtr &block);
    void streamStopped();

    bool isStreaming() const;

private:
    std::vector<std::shared_ptr<IqSink>> snapshot() const;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IqSink>> sinks_;
    bool streaming_ = false;
    StreamInfo info_;
};

} // namespace core

#endif
