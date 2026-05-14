#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace image_buffer {

struct FrameDescriptor {
    uint64_t frameId = 0;
    uint64_t timestampNs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t payloadSize = 0;
    uint32_t sequence = 0;
    std::string pixelFormat;
};

struct RawFrameView {
    FrameDescriptor descriptor;
    const uint8_t *data = nullptr;
    size_t size = 0;
};

struct StoredFrameView {
    FrameDescriptor descriptor;
    const uint8_t *data = nullptr;
    size_t size = 0;
};

} // namespace image_buffer
