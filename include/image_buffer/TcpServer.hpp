#pragma once

#include "image_buffer/FrameTypes.hpp"
#include "image_buffer/RingBuffer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace image_buffer {

struct TcpServerSettings {
    std::string host = "0.0.0.0";
    uint16_t port = 5003;
    uint32_t listenBacklog = 8;
};

class TcpServer {
public:
    TcpServer(RingBuffer &ringBuffer, TcpServerSettings settings);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    void start();
    void stop();

private:
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    void acceptLoop();
    void handleClient(int clientFd);
    void handleCommand(int clientFd, const std::string &commandLine);
    void sendFrames(int clientFd, const std::vector<FrameSnapshot> &frames);
    void sendError(int clientFd, const std::string &message);
    void pruneClientThreadsLocked();
    size_t activeClientCountLocked() const;
    void unregisterClientFd(int clientFd);

    RingBuffer &ringBuffer_;
    TcpServerSettings settings_;

    std::atomic<bool> running_{false};
    int serverFd_ = -1;
    std::thread acceptThread_;
    std::mutex clientThreadsMutex_;
    std::vector<ClientThread> clientThreads_;
    std::vector<int> clientFds_;
};

} // namespace image_buffer
