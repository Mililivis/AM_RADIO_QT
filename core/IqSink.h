#ifndef CORE_IQ_SINK_H
#define CORE_IQ_SINK_H

#include "IqTypes.h"

namespace core
{

/// @brief consumer of the IQ stream (receiver, spectrum analyzer, recorder, network streamer...)
///
/// Calls arrive from one thread at a time and in order: onStreamStart, onBlock..., onStreamStop.
/// A sink that may be slow must be wrapped into AsyncSink so it never blocks the USB thread.
/// Retuning while streaming is visible as a change of IqBlock::centerHz,
/// lost data as a jump of IqBlock::firstSample.
class IqSink
{
public:
    virtual ~IqSink() = default;

    virtual void onStreamStart(const StreamInfo &) {}
    virtual void onBlock(const IqBlockPtr &block) = 0;
    virtual void onStreamStop() {}
};

} // namespace core

#endif
