#include "image_buffer/CaptureEngine.hpp"
#include "image_buffer/TcpServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gStopRequested{false};

void handleSignal(int)
{
    gStopRequested.store(true);
}

int32_t readIntArg(const char *value, const std::string &name)
{
    try {
        return std::stoi(value);
    } catch (const std::exception &) {
        throw std::runtime_error("invalid integer for " + name + ": " + value);
    }
}

float readFloatArg(const char *value, const std::string &name)
{
    try {
        return std::stof(value);
    } catch (const std::exception &) {
        throw std::runtime_error("invalid float for " + name + ": " + value);
    }
}

void printUsage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --camera-id ID         Open a specific libcamera camera id\n"
        << "  --exposure-us VALUE    Fixed exposure time in microseconds (default: 8000)\n"
        << "  --gain VALUE           Fixed analogue gain (default: 1.0)\n"
        << "  --frame-us VALUE       Fixed frame duration in microseconds (default: 16666)\n"
        << "  --buffers VALUE        Number of libcamera buffers (default: 6)\n"
        << "  --ring-frames VALUE    Number of preallocated ring buffer frames (default: 150)\n"
        << "  --stats-every VALUE    Print one stats line every N frames (default: 60)\n"
        << "  --tcp-host ADDRESS     TCP bind address (default: 0.0.0.0)\n"
        << "  --tcp-port VALUE       TCP port (default: 8000)\n"
        << "  --no-tcp               Disable TCP server\n"
        << "  --help                 Show this help\n";
}

} // namespace

int main(int argc, char **argv)
{
    image_buffer::CaptureSettings settings;
    image_buffer::TcpServerSettings tcpSettings;
    bool tcpEnabled = true;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            auto requireValue = [&](const std::string &option) -> const char * {
                if (i + 1 >= argc) {
                    throw std::runtime_error("missing value for " + option);
                }
                return argv[++i];
            };

            if (arg == "--camera-id") {
                settings.cameraId = requireValue(arg);
            } else if (arg == "--exposure-us") {
                settings.exposureTimeUs = readIntArg(requireValue(arg), arg);
            } else if (arg == "--gain") {
                settings.analogueGain = readFloatArg(requireValue(arg), arg);
            } else if (arg == "--frame-us") {
                settings.frameDurationUs = readIntArg(requireValue(arg), arg);
            } else if (arg == "--buffers") {
                settings.bufferCount = static_cast<uint32_t>(readIntArg(requireValue(arg), arg));
            } else if (arg == "--ring-frames") {
                settings.ringBufferFrames = static_cast<uint32_t>(readIntArg(requireValue(arg), arg));
            } else if (arg == "--stats-every") {
                settings.statsIntervalFrames = static_cast<uint32_t>(readIntArg(requireValue(arg), arg));
            } else if (arg == "--tcp-host") {
                tcpSettings.host = requireValue(arg);
            } else if (arg == "--tcp-port") {
                tcpSettings.port = static_cast<uint16_t>(readIntArg(requireValue(arg), arg));
            } else if (arg == "--no-tcp") {
                tcpEnabled = false;
            } else if (arg == "--help") {
                printUsage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("unknown option: " + arg);
            }
        }

        image_buffer::CaptureEngine engine(settings);
        std::unique_ptr<image_buffer::TcpServer> tcpServer;

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        engine.start();
        if (tcpEnabled) {
            tcpServer = std::make_unique<image_buffer::TcpServer>(engine.ringBuffer(), tcpSettings);
            tcpServer->start();
        }

        while (!gStopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (tcpServer) {
            tcpServer->stop();
        }
        engine.stop();
        return gStopRequested.load() ? 130 : 0;
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << std::endl;
        return 1;
    }
}
