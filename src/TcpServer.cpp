#include "image_buffer/TcpServer.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace image_buffer {
namespace {

constexpr uint32_t kFrameMagic = 0x314d5849; // "IMX1" in little-endian memory.
constexpr size_t kMaxCommandBytes = 256;

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

    std::lock_guard<std::mutex> lock(clientThreadsMutex_);
    for (auto &thread : clientThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    clientThreads_.clear();
}

void TcpServer::acceptLoop()
{
    while (running_.load()) {
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
        timeout.tv_sec = 1;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        clientThreads_.emplace_back(&TcpServer::handleClient, this, clientFd);
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

} // namespace image_buffer
