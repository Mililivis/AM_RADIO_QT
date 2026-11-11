#ifndef CORE_ASYNC_SINK_H
#define CORE_ASYNC_SINK_H

#include "IqSink.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace core
{

/// @brief runs a sink in its own thread behind a bounded queue.
///
/// The producer (USB thread) only enqueues and never waits. If the inner sink is too slow,
/// the oldest queued blocks are dropped; the inner sink sees the loss as a jump of firstSample.
/// Stream start/stop events are never dropped.
class AsyncSink : public IqSink
{
public:
    AsyncSink(std::shared_ptr<IqSink> inner, double maxQueueSeconds, std::string name);
    ~AsyncSink() override;

    void onStreamStart(const StreamInfo &info) override;
    void onBlock(const IqBlockPtr &block) override;
    void onStreamStop() override;

    /// @brief wait until everything queued so far was processed by the inner sink
    void flush();

    const std::shared_ptr<IqSink> &inner() const { return inner_; }
    uint64_t droppedSamples() const { return droppedSamples_; }
    const std::string &name() const { return name_; }

private:
    struct Event
    {
        enum class Type
        {
            Start,
            Block,
            Stop
        } type;
        StreamInfo info;
        IqBlockPtr block;
    };

    void push(Event event);
    void run();

    std::shared_ptr<IqSink> inner_;
    double maxQueueSeconds_;
    std::string name_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idle_;
    std::deque<Event> queue_;
    double queuedSeconds_ = 0.0;
    bool busy_ = false;
    bool quit_ = false;
    std::atomic<uint64_t> droppedSamples_{0};
    std::thread thread_;
};

} // namespace core

#endif
