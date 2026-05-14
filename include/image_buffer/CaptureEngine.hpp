#pragma once

#include "image_buffer/FrameTypes.hpp"
#include "image_buffer/RingBuffer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
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
    uint32_t ringBufferFrames = 150;
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
    RingBuffer &ringBuffer() { return ringBuffer_; }
    const RingBuffer &ringBuffer() const { return ringBuffer_; }

private:
    struct MappedPlane {
        uint8_t *data = nullptr;
        size_t length = 0;
    };

    struct MappedBuffer {
        const libcamera::FrameBuffer *buffer = nullptr;
        std::vector<MappedPlane> planes;
    };

    void openCamera();
    void configureCamera();
    void allocateBuffers();
    void mapBuffers();
    void unmapBuffers();
    void createRequests();
    void applyManualControls(libcamera::ControlList &controls) const;
    void handleRequest(libcamera::Request *request);
    RawFrameView makeRawFrameView(const libcamera::FrameBuffer &buffer, uint64_t frameId) const;
    const MappedBuffer *findMappedBuffer(const libcamera::FrameBuffer &buffer) const;
    void printFrameStats(const FrameDescriptor &frame);

    CaptureSettings settings_;

    std::unique_ptr<libcamera::CameraManager> cameraManager_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> configuration_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *rawStream_ = nullptr;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    std::vector<MappedBuffer> mappedBuffers_;
    RingBuffer ringBuffer_;

    std::string pixelFormat_;
    uint32_t stride_ = 0;
    uint32_t payloadCapacity_ = 0;

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
