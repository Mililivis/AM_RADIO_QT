#ifndef NET_NETWORK_IQ_SINK_H
#define NET_NETWORK_IQ_SINK_H

#include "IqSink.h"
#include "RtlTcpServer.h"

#include <memory>

namespace net
{

/// @brief adapter: plugs the rtl_tcp server into the IQ pipeline as an ordinary sink.
/// This is the only place where the network module touches the core.
class NetworkIqSink : public core::IqSink
{
public:
    explicit NetworkIqSink(std::shared_ptr<RtlTcpServer> server)
        : server_(std::move(server))
    {
    }

    void onBlock(const core::IqBlockPtr &block) override
    {
        server_->send(block->raw.data(), block->raw.size());
    }

    const std::shared_ptr<RtlTcpServer> &server() const { return server_; }

private:
    std::shared_ptr<RtlTcpServer> server_;
};

} // namespace net

#endif
