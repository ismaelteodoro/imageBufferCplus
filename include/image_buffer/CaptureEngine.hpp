#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

namespace image_buffer {

struct CaptureSettings {
    std::string cameraId;
    uint32_t width = 1456;
    uint32_t height = 1088;
    int64_t frameDurationUs = 16666;
    int32_t exposureTimeUs = 8000;
    float analogueGain = 1.0f;
    uint32_t bufferCount = 6;
    uint32_t statsIntervalFrames = 60;
};

struct FrameInfo {
    uint64_t frameId = 0;
    uint64_t timestampNs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t payloadSize = 0;
    uint32_t sequence = 0;
};

class CaptureEngine {
public:
    explicit CaptureEngine(CaptureSettings settings);
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine &) = delete;
    CaptureEngine &operator=(const CaptureEngine &) = delete;

    void start();
    void stop();
    void wait();

private:
    void openCamera();
    void configureCamera();
    void allocateBuffers();
    void createRequests();
    void applyManualControls(libcamera::ControlList &controls) const;
    void handleRequest(libcamera::Request *request);
    void printFrameStats(const FrameInfo &frame);

    CaptureSettings settings_;

    std::unique_ptr<libcamera::CameraManager> cameraManager_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> configuration_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *rawStream_ = nullptr;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frameCounter_{0};
    std::atomic<uint64_t> droppedFrames_{0};

    std::mutex completionMutex_;
    std::condition_variable completionCv_;
    bool stopped_ = false;

    std::chrono::steady_clock::time_point statsWallStart_;
    uint64_t statsFrameStart_ = 0;
    uint64_t lastTimestampNs_ = 0;
};

} // namespace image_buffer
