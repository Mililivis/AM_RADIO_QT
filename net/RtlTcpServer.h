#ifndef NET_RTL_TCP_SERVER_H
#define NET_RTL_TCP_SERVER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net
{

/// @brief TCP server speaking the rtl_tcp protocol, so any SDR program on another host
/// (SDR++, GQRX, SDR#, HDSDR, GNU Radio osmosdr source...) can receive the IQ stream.
///
/// Protocol: on connect the server sends a 12-byte header {"RTL0", tuner type, gain count}
/// (big-endian), then a continuous stream of unsigned 8-bit interleaved I/Q.
/// Clients may send 5-byte commands {uint8 code, uint32 parameter big-endian}
/// (1 = frequency, 2 = sample rate, 3 = gain mode, 4 = gain in 0.1 dB, ...),
/// which are passed to the command handler.
///
/// Independent of the rest of the application: plain POSIX sockets, one thread.
/// send() never blocks: every client has its own bounded queue, a slow client loses
/// whole chunks (counted) instead of slowing down the radio or other clients.
class RtlTcpServer
{
public:
    struct Config
    {
        std::string bindAddress = "0.0.0.0";
        uint16_t port = 1234;
        size_t maxQueueBytes = 16u << 20;   // per client, ~3 s at 2.4 MS/s
        uint32_t tunerType = 6;             // RTLSDR_TUNER_R828D
        uint32_t gainCount = 29;
    };

    struct Command
    {
        uint8_t code;
        uint32_t param;
    };

    struct ClientInfo
    {
        std::string address;
        uint64_t bytesSent = 0;
        uint64_t bytesDropped = 0;
    };

    using CommandHandler = std::function<void(const std::string &client, const Command &command)>;

    explicit RtlTcpServer(Config config);
    ~RtlTcpServer();

    RtlTcpServer(const RtlTcpServer &) = delete;
    RtlTcpServer &operator=(const RtlTcpServer &) = delete;

    bool start(std::string &error);
    void stop();
    bool isRunning() const { return running_; }
    const Config &config() const { return config_; }

    /// @brief handler is called from the server thread
    void setCommandHandler(CommandHandler handler);

    /// @brief queue cu8 data for all connected clients (thread-safe, non-blocking)
    void send(const uint8_t *data, size_t len);

    std::vector<ClientInfo> clients() const;

private:
    struct Client
    {
        int fd = -1;
        ClientInfo info;
        std::deque<std::shared_ptr<const std::vector<uint8_t>>> queue;
        size_t queuedBytes = 0;
        size_t offset = 0;              // bytes of queue.front() already sent
        std::vector<uint8_t> commandBuffer;
    };

    void run();
    void acceptClient();
    bool readCommands(Client &client);
    bool writeQueued(Client &client);
    void wake();

    Config config_;
    int listenFd_ = -1;
    int wakePipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Client>> clients_;
    CommandHandler handler_;
};

} // namespace net

#endif
