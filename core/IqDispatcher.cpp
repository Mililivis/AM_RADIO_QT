#include "IqDispatcher.h"

#include <algorithm>

namespace core
{

void IqDispatcher::addSink(const std::shared_ptr<IqSink> &sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end())
    {
        return;
    }
    sinks_.push_back(sink);
    if (streaming_)
    {
        sink->onStreamStart(info_);
    }
}

void IqDispatcher::removeSink(const std::shared_ptr<IqSink> &sink)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(sinks_.begin(), sinks_.end(), sink);
    if (it == sinks_.end())
    {
        return;
    }
    if (streaming_)
    {
        sink->onStreamStop();
    }
    sinks_.erase(it);
}

void IqDispatcher::streamStarted(const StreamInfo &info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    streaming_ = true;
    info_ = info;
    for (auto &s : sinks_)
    {
        s->onStreamStart(info);
    }
}

void IqDispatcher::dispatch(const IqBlockPtr &block)
{
    // Sinks are called outside the lock so add/remove never waits for a block to be processed
    for (auto &s : snapshot())
    {
        s->onBlock(block);
    }
}

void IqDispatcher::streamStopped()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streaming_)
    {
        return;
    }
    streaming_ = false;
    for (auto &s : sinks_)
    {
        s->onStreamStop();
    }
}

bool IqDispatcher::isStreaming() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return streaming_;
}

std::vector<std::shared_ptr<IqSink>> IqDispatcher::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sinks_;
}

} // namespace core
