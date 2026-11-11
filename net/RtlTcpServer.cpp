#include "RtlTcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{

namespace
{
constexpr size_t commandSize = 5;

void putBigEndian(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void setNonBlocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
} // namespace

RtlTcpServer::RtlTcpServer(Config config)
    : config_(std::move(config))
{
}

RtlTcpServer::~RtlTcpServer()
{
    stop();
}

void RtlTcpServer::setCommandHandler(CommandHandler handler)
{
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

bool RtlTcpServer::start(std::string &error)
{
    if (running_)
    {
        return true;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
    {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.bindAddress.c_str(), &addr.sin_addr) != 1)
    {
        error = "bad bind address " + config_.bindAddress;
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(listenFd_, 4) < 0)
    {
        error = "port " + std::to_string(config_.port) + ": " + std::strerror(errno);
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    setNonBlocking(listenFd_);

    if (pipe(wakePipe_) < 0)
    {
        error = std::string("pipe: ") + std::strerror(errno);
        close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    setNonBlocking(wakePipe_[0]);
    setNonBlocking(wakePipe_[1]);

    running_ = true;
    thread_ = std::thread(&RtlTcpServer::run, this);
    return true;
}

void RtlTcpServer::stop()
{
    if (!running_)
    {
        return;
    }
    running_ = false;
    wake();
    thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &c : clients_)
    {
        close(c->fd);
    }
    clients_.clear();
    close(listenFd_);
    close(wakePipe_[0]);
    close(wakePipe_[1]);
    listenFd_ = wakePipe_[0] = wakePipe_[1] = -1;
}

void RtlTcpServer::wake()
{
    uint8_t b = 1;
    [[maybe_unused]] ssize_t n = write(wakePipe_[1], &b, 1);
}

void RtlTcpServer::send(const uint8_t *data, size_t len)
{
    if (!running_ || len == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (clients_.empty())
    {
        return;
    }

    // One copy shared by all clients
    auto chunk = std::make_shared<const std::vector<uint8_t>>(data, data + len);
    bool needWake = false;

    for (auto &c : clients_)
    {
        if (c->queuedBytes + len > config_.maxQueueBytes)
        {
            // Drop the oldest whole chunks that are not being sent right now (keeps I/Q alignment)
            while (c->queue.size() > 1 && c->queuedBytes + len > config_.maxQueueBytes)
            {
                auto victim = c->queue.begin() + 1;
                c->queuedBytes -= (*victim)->size();
                c->info.bytesDropped += (*victim)->size();
                c->queue.erase(victim);
            }
            if (c->queuedBytes + len > config_.maxQueueBytes)
            {
                c->info.bytesDropped += len;
                continue;
            }
        }
        needWake |= c->queue.empty();
        c->queue.push_back(chunk);
        c->queuedBytes += len;
    }

    if (needWake)
    {
        wake();
    }
}

std::vector<RtlTcpServer::ClientInfo> RtlTcpServer::clients() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClientInfo> list;
    for (const auto &c : clients_)
    {
        list.push_back(c->info);
    }
    return list;
}

void RtlTcpServer::acceptClient()
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int fd = accept(listenFd_, reinterpret_cast<sockaddr *>(&addr), &len);
    if (fd < 0)
    {
        return;
    }
    setNonBlocking(fd);
    int sndbuf = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    auto client = std::make_unique<Client>();
    client->fd = fd;
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    client->info.address = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));

    // rtl_tcp dongle info header
    auto header = std::make_shared<std::vector<uint8_t>>(12);
    std::memcpy(header->data(), "RTL0", 4);
    putBigEndian(header->data() + 4, config_.tunerType);
    putBigEndian(header->data() + 8, config_.gainCount);
    client->queue.push_back(header);
    client->queuedBytes = header->size();

    std::lock_guard<std::mutex> lock(mutex_);
    clients_.push_back(std::move(client));
}

bool RtlTcpServer::readCommands(Client &client)
{
    uint8_t buf[256];
    for (;;)
    {
        ssize_t n = recv(client.fd, buf, sizeof(buf), 0);
        if (n == 0)
        {
            return false;   // disconnected
        }
        if (n < 0)
        {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        client.commandBuffer.insert(client.commandBuffer.end(), buf, buf + n);

        CommandHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = handler_;
        }
        size_t pos = 0;
        for (; pos + commandSize <= client.commandBuffer.size(); pos += commandSize)
        {
            const uint8_t *c = client.commandBuffer.data() + pos;
            Command cmd{c[0], (uint32_t(c[1]) << 24) | (uint32_t(c[2]) << 16) | (uint32_t(c[3]) << 8) | c[4]};
            if (handler)
            {
                handler(client.info.address, cmd);
            }
        }
        client.commandBuffer.erase(client.commandBuffer.begin(),
                                   client.commandBuffer.begin() + static_cast<std::ptrdiff_t>(pos));
    }
}

bool RtlTcpServer::writeQueued(Client &client)
{
    for (;;)
    {
        std::shared_ptr<const std::vector<uint8_t>> chunk;
        size_t offset;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (client.queue.empty())
            {
                return true;
            }
            chunk = client.queue.front();
            offset = client.offset;
        }

        ssize_t n = ::send(client.fd, chunk->data() + offset, chunk->size() - offset, MSG_NOSIGNAL);
        if (n < 0)
        {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        client.offset += static_cast<size_t>(n);
        client.info.bytesSent += static_cast<uint64_t>(n);
        if (client.offset == chunk->size())
        {
            client.queue.pop_front();
            client.queuedBytes -= chunk->size();
            client.offset = 0;
        }
        else
        {
            return true;   // socket buffer full, wait for POLLOUT
        }
    }
}

void RtlTcpServer::run()
{
    std::vector<pollfd> fds;
    std::vector<Client *> polled;

    while (running_)
    {
        fds.clear();
        polled.clear();
        fds.push_back({listenFd_, POLLIN, 0});
        fds.push_back({wakePipe_[0], POLLIN, 0});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &c : clients_)
            {
                short events = POLLIN;
                if (!c->queue.empty())
                {
                    events |= POLLOUT;
                }
                fds.push_back({c->fd, events, 0});
                polled.push_back(c.get());
            }
        }

        if (poll(fds.data(), fds.size(), 500) < 0 && errno != EINTR)
        {
            break;
        }
        if (!running_)
        {
            break;
        }

        if (fds[1].revents & POLLIN)
        {
            uint8_t drain[64];
            while (read(wakePipe_[0], drain, sizeof(drain)) > 0)
            {
            }
        }

        std::vector<Client *> dead;
        for (size_t i = 0; i < polled.size(); ++i)
        {
            const short ev = fds[i + 2].revents;
            Client &c = *polled[i];
            bool alive = !(ev & (POLLERR | POLLHUP | POLLNVAL));
            if (alive && (ev & POLLIN))
            {
                alive = readCommands(c);
            }
            if (alive && (ev & POLLOUT))
            {
                alive = writeQueued(c);
            }
            if (!alive)
            {
                dead.push_back(&c);
            }
        }

        if (!dead.empty())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Client *d : dead)
            {
                close(d->fd);
                for (auto it = clients_.begin(); it != clients_.end(); ++it)
                {
                    if (it->get() == d)
                    {
                        clients_.erase(it);
                        break;
                    }
                }
            }
        }

        if (fds[0].revents & POLLIN)
        {
            acceptClient();
        }
    }
}

} // namespace net
