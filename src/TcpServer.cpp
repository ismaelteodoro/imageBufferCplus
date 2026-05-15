#include "image_buffer/TcpServer.hpp"

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace image_buffer {
namespace {

constexpr uint32_t kFrameMagic = 0x314d5849; // "IMX1" in little-endian memory.
constexpr size_t kMaxCommandBytes = 256;
constexpr size_t kMaxClients = 3;
constexpr size_t kMaxGetLastFrames = 150;
constexpr time_t kSocketTimeoutSeconds = 1;

#pragma pack(push, 1)
struct WireFrameHeader {
    uint32_t magic;
    uint64_t frameId;
    uint64_t timestampNs;
    uint32_t width;
    uint32_t height;
    uint32_t payloadSize;
};
#pragma pack(pop)

bool sendAll(int fd, const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t sent = 0;

    while (sent < size) {
        const ssize_t result = ::send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (result == 0) {
            return false;
        }

        sent += static_cast<size_t>(result);
    }

    return true;
}

std::string trimLine(std::string line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }

    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }

    return line.substr(start);
}

} // namespace

TcpServer::TcpServer(RingBuffer &ringBuffer, TcpServerSettings settings)
    : ringBuffer_(ringBuffer)
    , settings_(std::move(settings))
{
}

TcpServer::~TcpServer()
{
    stop();
}

void TcpServer::start()
{
    if (running_.exchange(true)) {
        return;
    }

    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        running_.store(false);
        throw std::runtime_error("failed to create TCP socket");
    }

    int enabled = 1;
    ::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(settings_.port);
    if (::inet_pton(AF_INET, settings_.host.c_str(), &address.sin_addr) != 1) {
        ::close(serverFd_);
        serverFd_ = -1;
        running_.store(false);
        throw std::runtime_error("invalid TCP bind address: " + settings_.host);
    }

    if (::bind(serverFd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        const std::string error = std::strerror(errno);
        ::close(serverFd_);
        serverFd_ = -1;
        running_.store(false);
        throw std::runtime_error("failed to bind TCP server: " + error);
    }

    if (::listen(serverFd_, static_cast<int>(settings_.listenBacklog)) < 0) {
        const std::string error = std::strerror(errno);
        ::close(serverFd_);
        serverFd_ = -1;
        running_.store(false);
        throw std::runtime_error("failed to listen on TCP socket: " + error);
    }

    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        clientThreads_.reserve(kMaxClients);
        clientFds_.reserve(kMaxClients);
    }

    acceptThread_ = std::thread(&TcpServer::acceptLoop, this);

    std::cout << "tcp server listening on " << settings_.host << ":" << settings_.port << std::endl;
}

void TcpServer::stop()
{
    if (!running_.exchange(false)) {
        return;
    }

    if (serverFd_ >= 0) {
        ::shutdown(serverFd_, SHUT_RDWR);
        ::close(serverFd_);
        serverFd_ = -1;
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    std::vector<int> clientFds;
    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        clientFds = clientFds_;
    }

    for (const int clientFd : clientFds) {
        ::shutdown(clientFd, SHUT_RDWR);
    }

    std::vector<ClientThread> clientThreads;
    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        clientThreads.swap(clientThreads_);
        clientFds_.clear();
    }

    for (auto &client : clientThreads) {
        if (client.thread.joinable()) {
            client.thread.join();
        }
    }
}

void TcpServer::acceptLoop()
{
    while (running_.load()) {
        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex_);
            pruneClientThreadsLocked();
        }

        pollfd serverPoll {};
        serverPoll.fd = serverFd_;
        serverPoll.events = POLLIN;
        const int pollResult = ::poll(&serverPoll, 1, static_cast<int>(kSocketTimeoutSeconds * 1000));
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (running_.load()) {
                std::cerr << "tcp poll failed: " << std::strerror(errno) << std::endl;
            }
            continue;
        }
        if (pollResult == 0) {
            continue;
        }
        if ((serverPoll.revents & POLLIN) == 0) {
            continue;
        }

        sockaddr_in clientAddress {};
        socklen_t clientLength = sizeof(clientAddress);
        const int clientFd = ::accept(serverFd_, reinterpret_cast<sockaddr *>(&clientAddress), &clientLength);
        if (clientFd < 0) {
            if (running_.load() && errno != EINTR) {
                std::cerr << "tcp accept failed: " << std::strerror(errno) << std::endl;
            }
            continue;
        }

        timeval timeout {};
        timeout.tv_sec = kSocketTimeoutSeconds;
        if (::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            std::cerr << "tcp failed to set receive timeout: " << std::strerror(errno) << std::endl;
        }
        if (::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            std::cerr << "tcp failed to set send timeout: " << std::strerror(errno) << std::endl;
        }

        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        if (activeClientCountLocked() >= kMaxClients) {
            ::close(clientFd);
            continue;
        }

        auto finished = std::make_shared<std::atomic<bool>>(false);
        clientFds_.push_back(clientFd);
        clientThreads_.push_back({ std::thread([this, clientFd, finished]() {
                                       try {
                                           handleClient(clientFd);
                                       } catch (const std::exception &error) {
                                           std::cerr << "tcp client handler failed: " << error.what() << std::endl;
                                           unregisterClientFd(clientFd);
                                           ::close(clientFd);
                                       } catch (...) {
                                           std::cerr << "tcp client handler failed with unknown error" << std::endl;
                                           unregisterClientFd(clientFd);
                                           ::close(clientFd);
                                       }
                                       finished->store(true);
                                   }),
                                   finished });
    }
}

void TcpServer::handleClient(int clientFd)
{
    std::string line;
    line.reserve(kMaxCommandBytes);

    char byte = '\0';
    while (running_.load()) {
        const ssize_t result = ::recv(clientFd, &byte, 1, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }

        if (result == 0) {
            break;
        }

        line.push_back(byte);
        if (byte == '\n') {
            handleCommand(clientFd, trimLine(line));
            line.clear();
        } else if (line.size() > kMaxCommandBytes) {
            sendError(clientFd, "command too long");
            break;
        }
    }

    unregisterClientFd(clientFd);
    ::close(clientFd);
}

void TcpServer::handleCommand(int clientFd, const std::string &commandLine)
{
    if (commandLine.empty()) {
        return;
    }

    std::istringstream parser(commandLine);
    std::string command;
    parser >> command;

    if (command == "GET_LATEST") {
        FrameSnapshot frame;
        if (!ringBuffer_.latestSnapshot(frame)) {
            sendError(clientFd, "no frames available");
            return;
        }
        sendFrames(clientFd, { std::move(frame) });
        return;
    }

    if (command == "GET_FRAME") {
        uint64_t frameId = 0;
        if (!(parser >> frameId)) {
            sendError(clientFd, "usage: GET_FRAME ID");
            return;
        }

        FrameSnapshot frame;
        if (!ringBuffer_.findSnapshot(frameId, frame)) {
            sendError(clientFd, "frame not available");
            return;
        }
        sendFrames(clientFd, { std::move(frame) });
        return;
    }

    if (command == "GET_LAST") {
        size_t count = 0;
        if (!(parser >> count)) {
            sendError(clientFd, "usage: GET_LAST N");
            return;
        }

        if (count > kMaxGetLastFrames) {
            sendError(clientFd, "GET_LAST limit is 150 frames");
            return;
        }

        const std::vector<FrameSnapshot> frames = ringBuffer_.lastSnapshots(count);
        if (frames.empty()) {
            sendError(clientFd, "no frames available");
            return;
        }
        sendFrames(clientFd, frames);
        return;
    }

    if (command == "PING") {
        const std::string response = "OK PONG\n";
        sendAll(clientFd, response.data(), response.size());
        return;
    }

    sendError(clientFd, "unknown command");
}

void TcpServer::sendFrames(int clientFd, const std::vector<FrameSnapshot> &frames)
{
    std::ostringstream prefix;
    prefix << "OK " << frames.size() << "\n";
    const std::string prefixText = prefix.str();
    if (!sendAll(clientFd, prefixText.data(), prefixText.size())) {
        return;
    }

    for (const FrameSnapshot &frame : frames) {
        WireFrameHeader header {};
        header.magic = kFrameMagic;
        header.frameId = frame.descriptor.frameId;
        header.timestampNs = frame.descriptor.timestampNs;
        header.width = frame.descriptor.width;
        header.height = frame.descriptor.height;
        header.payloadSize = static_cast<uint32_t>(frame.payload.size());

        if (!sendAll(clientFd, &header, sizeof(header))) {
            return;
        }

        if (!frame.payload.empty() && !sendAll(clientFd, frame.payload.data(), frame.payload.size())) {
            return;
        }
    }
}

void TcpServer::sendError(int clientFd, const std::string &message)
{
    const std::string response = "ERR " + message + "\n";
    sendAll(clientFd, response.data(), response.size());
}

void TcpServer::pruneClientThreadsLocked()
{
    auto client = clientThreads_.begin();
    while (client != clientThreads_.end()) {
        if (client->finished && client->finished->load()) {
            if (client->thread.joinable()) {
                client->thread.join();
            }
            client = clientThreads_.erase(client);
            continue;
        }
        ++client;
    }
}

size_t TcpServer::activeClientCountLocked() const
{
    size_t activeClients = 0;
    for (const ClientThread &client : clientThreads_) {
        if (!client.finished || !client.finished->load()) {
            ++activeClients;
        }
    }
    return activeClients;
}

void TcpServer::unregisterClientFd(int clientFd)
{
    std::lock_guard<std::mutex> lock(clientThreadsMutex_);
    const auto client = std::find(clientFds_.begin(), clientFds_.end(), clientFd);
    if (client != clientFds_.end()) {
        clientFds_.erase(client);
    }
}

} // namespace image_buffer
