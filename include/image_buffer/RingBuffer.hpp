#pragma once

#include "image_buffer/FrameTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace image_buffer {

struct RingBufferStats {
    uint64_t framesWritten = 0;
    uint64_t overwrites = 0;
    size_t capacityFrames = 0;
    size_t usedFrames = 0;
    size_t payloadCapacity = 0;
};

class RingBuffer {
public:
    RingBuffer() = default;
    RingBuffer(size_t frameCapacity, size_t payloadCapacity);

    RingBuffer(const RingBuffer &) = delete;
    RingBuffer &operator=(const RingBuffer &) = delete;

    void reset(size_t frameCapacity, size_t payloadCapacity);
    void write(const RawFrameView &frame);

    bool latest(StoredFrameView &frame) const;
    bool find(uint64_t frameId, StoredFrameView &frame) const;
    RingBufferStats stats() const;

private:
    struct Slot {
        FrameDescriptor descriptor;
        std::vector<uint8_t> payload;
        bool valid = false;
    };

    size_t physicalIndexForFrameId(uint64_t frameId) const;

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    size_t writeIndex_ = 0;
    size_t usedFrames_ = 0;
    size_t payloadCapacity_ = 0;
    uint64_t framesWritten_ = 0;
    uint64_t overwrites_ = 0;
};

} // namespace image_buffer
