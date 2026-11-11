#include "AsyncSink.h"

namespace core
{

namespace
{
double blockSeconds(const IqBlockPtr &b)
{
    return b && b->sampleRate > 0.0 ? static_cast<double>(b->size()) / b->sampleRate : 0.0;
}
} // namespace

AsyncSink::AsyncSink(std::shared_ptr<IqSink> inner, double maxQueueSeconds, std::string name)
    : inner_(std::move(inner)), maxQueueSeconds_(maxQueueSeconds), name_(std::move(name))
{
    thread_ = std::thread(&AsyncSink::run, this);
}

AsyncSink::~AsyncSink()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void AsyncSink::onStreamStart(const StreamInfo &info)
{
    push({Event::Type::Start, info, nullptr});
}

void AsyncSink::onBlock(const IqBlockPtr &block)
{
    push({Event::Type::Block, {}, block});
}

void AsyncSink::onStreamStop()
{
    push({Event::Type::Stop, {}, nullptr});
}

void AsyncSink::push(Event event)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.type == Event::Type::Block)
        {
            queuedSeconds_ += blockSeconds(event.block);

            // Too slow: drop oldest blocks (control events stay)
            for (auto it = queue_.begin(); queuedSeconds_ > maxQueueSeconds_ && it != queue_.end();)
            {
                if (it->type == Event::Type::Block)
                {
                    queuedSeconds_ -= blockSeconds(it->block);
                    droppedSamples_ += it->block->size();
                    it = queue_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        queue_.push_back(std::move(event));
    }
    cv_.notify_one();
}

void AsyncSink::flush()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [&] { return (queue_.empty() && !busy_) || quit_; });
}

void AsyncSink::run()
{
    for (;;)
    {
        Event event;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (queue_.empty())
            {
                return;   // quit_ and nothing left
            }
            event = std::move(queue_.front());
            queue_.pop_front();
            if (event.type == Event::Type::Block)
            {
                queuedSeconds_ -= blockSeconds(event.block);
            }
            busy_ = true;
        }

        switch (event.type)
        {
        case Event::Type::Start:
            inner_->onStreamStart(event.info);
            break;
        case Event::Type::Block:
            inner_->onBlock(event.block);
            break;
        case Event::Type::Stop:
            inner_->onStreamStop();
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (queue_.empty())
            {
                idle_.notify_all();
            }
        }
    }
}

} // namespace core
