#pragma once
// LifeCore/Metal/Handles.h
// Opaque GPU resource handles. Simulation modules only ever see these; the
// mapping to actual MTLTexture / MTLBuffer objects lives inside ResourcePool.
// This keeps module headers pure C++ and makes resource lifetime auditable.

#include <cstdint>

namespace life {

struct TextureHandle {
    uint32_t index = 0;      // slot index + 1 (0 == invalid)
    uint32_t generation = 0; // guards against stale handles after release

    bool valid() const { return index != 0; }
    bool operator==(const TextureHandle&) const = default;
};

struct BufferHandle {
    uint32_t index = 0;
    uint32_t generation = 0;

    bool valid() const { return index != 0; }
    bool operator==(const BufferHandle&) const = default;
};

} // namespace life
