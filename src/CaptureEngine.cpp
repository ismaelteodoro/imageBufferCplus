#include "image_buffer/CaptureEngine.hpp"

#include <algorithm>
#include <csignal>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <libcamera/control_ids.h>
#include <libcamera/formats.h>

namespace image_buffer {
namespace {

std::string statusToString(libcamera::CameraConfiguration::Status status)
{
    switch (status) {
    case libcamera::CameraConfiguration::Valid:
        return "valid";
    case libcamera::CameraConfiguration::Adjusted:
        return "adjusted";
    case libcamera::CameraConfiguration::Invalid:
        return "invalid";
    }

    return "unknown";
}

uint32_t payloadBytes(const libcamera::FrameBuffer &buffer)
{
    uint32_t total = 0;
    for (const auto &plane : buffer.metadata().planes()) {
        total += plane.bytesused;
    }
    return total;
}

bool isPackedRaw10(const libcamera::PixelFormat &format)
{
    return format == libcamera::formats::SRGGB10_CSI2P ||
           format == libcamera::formats::SBGGR10_CSI2P ||
           format == libcamera::formats::SGRBG10_CSI2P ||
           format == libcamera::formats::SGBRG10_CSI2P;
}

} // namespace

CaptureEngine::CaptureEngine(CaptureSettings settings)
    : settings_(std::move(settings))
{
}

CaptureEngine::~CaptureEngine()
{
    stop();
}

void CaptureEngine::start()
{
    openCamera();
    configureCamera();
    allocateBuffers();
    createRequests();

    libcamera::ControlList controls(camera_->controls());
    applyManualControls(controls);

    camera_->requestCompleted.connect(this, &CaptureEngine::handleRequest);

    const int startResult = camera_->start(&controls);
    if (startResult < 0) {
        throw std::runtime_error("failed to start camera: " + std::to_string(startResult));
    }

    running_.store(true);
    stopped_ = false;
    statsWallStart_ = std::chrono::steady_clock::now();
    statsFrameStart_ = 0;

    for (const auto &request : requests_) {
        const int queueResult = camera_->queueRequest(request.get());
        if (queueResult < 0) {
            throw std::runtime_error("failed to queue request: " + std::to_string(queueResult));
        }
    }

    std::cout << "capture started: "
              << settings_.width << "x" << settings_.height
              << " SRGGB10_CSI2P, exposure=" << settings_.exposureTimeUs
              << "us, gain=" << settings_.analogueGain
              << ", frame_duration=" << settings_.frameDurationUs << "us"
              << std::endl;
}

void CaptureEngine::stop()
{
    const bool wasRunning = running_.exchange(false);

    if (camera_) {
        camera_->requestCompleted.disconnect(this, &CaptureEngine::handleRequest);
    }

    if (camera_ && wasRunning) {
        camera_->stop();
    }

    requests_.clear();
    allocator_.reset();

    if (camera_) {
        camera_->release();
        camera_.reset();
    }

    if (cameraManager_) {
        cameraManager_->stop();
        cameraManager_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(completionMutex_);
        stopped_ = true;
    }
    completionCv_.notify_all();
}

void CaptureEngine::wait()
{
    std::unique_lock<std::mutex> lock(completionMutex_);
    completionCv_.wait(lock, [this] { return stopped_; });
}

void CaptureEngine::openCamera()
{
    cameraManager_ = std::make_unique<libcamera::CameraManager>();
    const int startResult = cameraManager_->start();
    if (startResult < 0) {
        throw std::runtime_error("failed to start camera manager: " + std::to_string(startResult));
    }

    const auto cameras = cameraManager_->cameras();
    if (cameras.empty()) {
        throw std::runtime_error("no cameras found by libcamera");
    }

    if (!settings_.cameraId.empty()) {
        camera_ = cameraManager_->get(settings_.cameraId);
        if (!camera_) {
            throw std::runtime_error("camera id not found: " + settings_.cameraId);
        }
    } else {
        camera_ = cameras.front();
    }

    const int acquireResult = camera_->acquire();
    if (acquireResult < 0) {
        throw std::runtime_error("failed to acquire camera: " + std::to_string(acquireResult));
    }

    std::cout << "camera: " << camera_->id() << std::endl;
}

void CaptureEngine::configureCamera()
{
    configuration_ = camera_->generateConfiguration({ libcamera::StreamRole::Raw });
    if (!configuration_ || configuration_->empty()) {
        throw std::runtime_error("failed to generate raw camera configuration");
    }

    auto &streamConfig = configuration_->at(0);
    streamConfig.pixelFormat = libcamera::formats::SRGGB10_CSI2P;
    streamConfig.size = libcamera::Size(settings_.width, settings_.height);
    streamConfig.bufferCount = settings_.bufferCount;

    const auto validationStatus = configuration_->validate();
    if (validationStatus == libcamera::CameraConfiguration::Invalid) {
        throw std::runtime_error("invalid camera configuration");
    }

    if (!isPackedRaw10(streamConfig.pixelFormat) ||
        streamConfig.size.width != settings_.width ||
        streamConfig.size.height != settings_.height) {
        std::ostringstream error;
        error << "camera adjusted away from required packed RAW10 1456x1088 mode: "
              << streamConfig.toString()
              << " (" << statusToString(validationStatus) << ")";
        throw std::runtime_error(error.str());
    }

    const int configureResult = camera_->configure(configuration_.get());
    if (configureResult < 0) {
        throw std::runtime_error("failed to configure camera: " + std::to_string(configureResult));
    }

    rawStream_ = streamConfig.stream();
    if (!rawStream_) {
        throw std::runtime_error("raw stream was not created");
    }

    std::cout << "configuration: " << streamConfig.toString()
              << " (" << statusToString(validationStatus) << ")"
              << ", buffers=" << streamConfig.bufferCount
              << ", frame_size=" << streamConfig.frameSize
              << ", stride=" << streamConfig.stride
              << std::endl;
}

void CaptureEngine::allocateBuffers()
{
    allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);

    const int allocated = allocator_->allocate(rawStream_);
    if (allocated < 0) {
        throw std::runtime_error("failed to allocate frame buffers: " + std::to_string(allocated));
    }

    const auto &buffers = allocator_->buffers(rawStream_);
    if (buffers.empty()) {
        throw std::runtime_error("libcamera allocated zero frame buffers");
    }

    std::cout << "allocated buffers: " << buffers.size() << std::endl;
}

void CaptureEngine::createRequests()
{
    const auto &buffers = allocator_->buffers(rawStream_);
    requests_.reserve(buffers.size());

    for (const auto &buffer : buffers) {
        std::unique_ptr<libcamera::Request> request = camera_->createRequest();
        if (!request) {
            throw std::runtime_error("failed to create capture request");
        }

        const int addBufferResult = request->addBuffer(rawStream_, buffer.get());
        if (addBufferResult < 0) {
            throw std::runtime_error("failed to bind buffer to request: " + std::to_string(addBufferResult));
        }

        requests_.push_back(std::move(request));
    }
}

void CaptureEngine::applyManualControls(libcamera::ControlList &controls) const
{
    controls.set(libcamera::controls::AeEnable, false);
    controls.set(libcamera::controls::AwbEnable, false);
    controls.set(libcamera::controls::ExposureTime, settings_.exposureTimeUs);
    controls.set(libcamera::controls::AnalogueGain, settings_.analogueGain);
    controls.set(libcamera::controls::FrameDurationLimits,
                 { settings_.frameDurationUs, settings_.frameDurationUs });
}

void CaptureEngine::handleRequest(libcamera::Request *request)
{
    if (!request) {
        return;
    }

    if (!running_.load()) {
        return;
    }

    if (request->status() == libcamera::Request::RequestCancelled) {
        return;
    }

    libcamera::FrameBuffer *buffer = request->findBuffer(rawStream_);
    if (!buffer) {
        droppedFrames_.fetch_add(1);
        request->reuse(libcamera::Request::ReuseBuffers);
        camera_->queueRequest(request);
        return;
    }

    if (buffer->metadata().status != libcamera::FrameMetadata::FrameSuccess) {
        droppedFrames_.fetch_add(1);
        request->reuse(libcamera::Request::ReuseBuffers);
        camera_->queueRequest(request);
        return;
    }

    const uint64_t frameId = frameCounter_.fetch_add(1) + 1;
    FrameInfo frame;
    frame.frameId = frameId;
    frame.timestampNs = buffer->metadata().timestamp;
    frame.width = settings_.width;
    frame.height = settings_.height;
    frame.payloadSize = payloadBytes(*buffer);
    frame.sequence = buffer->metadata().sequence;

    printFrameStats(frame);

    request->reuse(libcamera::Request::ReuseBuffers);
    const int queueResult = camera_->queueRequest(request);
    if (queueResult < 0 && running_.load()) {
        droppedFrames_.fetch_add(1);
        std::cerr << "failed to requeue request: " << queueResult << std::endl;
    }
}

void CaptureEngine::printFrameStats(const FrameInfo &frame)
{
    const uint64_t previousTimestamp = lastTimestampNs_;
    lastTimestampNs_ = frame.timestampNs;

    if (frame.frameId == 1 || frame.frameId % settings_.statsIntervalFrames == 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - statsWallStart_).count();
        const uint64_t frames = frame.frameId - statsFrameStart_;
        const double fps = elapsed > 0.0 ? static_cast<double>(frames) / elapsed : 0.0;
        const double deltaMs = previousTimestamp == 0
                                   ? 0.0
                                   : static_cast<double>(frame.timestampNs - previousTimestamp) / 1'000'000.0;

        std::cout << "frame_id=" << frame.frameId
                  << " seq=" << frame.sequence
                  << " timestamp_ns=" << frame.timestampNs
                  << " delta_ms=" << deltaMs
                  << " payload=" << frame.payloadSize
                  << " fps=" << fps
                  << " dropped=" << droppedFrames_.load()
                  << std::endl;

        statsWallStart_ = now;
        statsFrameStart_ = frame.frameId;
    }
}

} // namespace image_buffer
