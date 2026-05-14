#include "image_buffer/RingBuffer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace image_buffer {

RingBuffer::RingBuffer(size_t frameCapacity, size_t payloadCapacity)
{
    reset(frameCapacity, payloadCapacity);
}

void RingBuffer::reset(size_t frameCapacity, size_t payloadCapacity)
{
    if (frameCapacity == 0) {
        throw std::runtime_error("ring buffer frame capacity must be greater than zero");
    }

    if (payloadCapacity == 0) {
        throw std::runtime_error("ring buffer payload capacity must be greater than zero");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    slots_.clear();
    slots_.resize(frameCapacity);
    for (auto &slot : slots_) {
        slot.payload.resize(payloadCapacity);
        slot.valid = false;
        slot.descriptor = {};
    }

    writeIndex_ = 0;
    usedFrames_ = 0;
    payloadCapacity_ = payloadCapacity;
    framesWritten_ = 0;
    overwrites_ = 0;
}

void RingBuffer::write(const RawFrameView &frame)
{
    if (!frame.data) {
        throw std::runtime_error("cannot write a null raw frame into the ring buffer");
    }

    if (frame.size > payloadCapacity_) {
        throw std::runtime_error("raw frame is larger than the ring buffer slot capacity");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    Slot &slot = slots_[writeIndex_];
    if (slot.valid) {
        ++overwrites_;
    }

    std::memcpy(slot.payload.data(), frame.data, frame.size);
    slot.descriptor = frame.descriptor;
    slot.descriptor.payloadSize = static_cast<uint32_t>(frame.size);
    slot.valid = true;

    writeIndex_ = (writeIndex_ + 1) % slots_.size();
    usedFrames_ = std::min(usedFrames_ + 1, slots_.size());
    ++framesWritten_;
}

bool RingBuffer::latest(StoredFrameView &frame) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (usedFrames_ == 0) {
        return false;
    }

    const size_t index = (writeIndex_ + slots_.size() - 1) % slots_.size();
    const Slot &slot = slots_[index];
    if (!slot.valid) {
        return false;
    }

    frame.descriptor = slot.descriptor;
    frame.data = slot.payload.data();
    frame.size = slot.descriptor.payloadSize;
    return true;
}

bool RingBuffer::find(uint64_t frameId, StoredFrameView &frame) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (usedFrames_ == 0) {
        return false;
    }

    const size_t index = physicalIndexForFrameId(frameId);
    const Slot &slot = slots_[index];
    if (!slot.valid || slot.descriptor.frameId != frameId) {
        return false;
    }

    frame.descriptor = slot.descriptor;
    frame.data = slot.payload.data();
    frame.size = slot.descriptor.payloadSize;
    return true;
}

RingBufferStats RingBuffer::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    RingBufferStats result;
    result.framesWritten = framesWritten_;
    result.overwrites = overwrites_;
    result.capacityFrames = slots_.size();
    result.usedFrames = usedFrames_;
    result.payloadCapacity = payloadCapacity_;
    return result;
}

size_t RingBuffer::physicalIndexForFrameId(uint64_t frameId) const
{
    if (slots_.empty() || frameId == 0) {
        return 0;
    }

    return static_cast<size_t>((frameId - 1) % slots_.size());
}

} // namespace image_buffer
