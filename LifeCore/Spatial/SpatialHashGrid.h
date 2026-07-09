#pragma once
// LifeCore/Spatial/SpatialHashGrid.h
// 2D uniform-grid neighbor index over a toroidal pixel-space world (design
// doc §9, Phase 4). All-pairs O(n²) interaction is banned (§9.2); consumers
// (Particle Life, Boids, later SPH) build this once per step and iterate the
// 3×3 cell neighborhood instead. cellSize should equal the interaction
// radius (§9.3).
//
// Build = counting sort, encoded as compute passes (see
// Shaders/Spatial/SpatialHash.metal):
//   hashClearCells → hashCount → hashScanInclusive × ceil(log2(numCells))
//   → hashFinalize → hashScatter → hashSortCells
// The final per-cell sort (gap-sequence insertion sort, no size cutoff)
// restores canonical ascending-index order after the nondeterministic atomic
// scatter, which is what keeps consumer float accumulation bit-deterministic
// for a given seed — even inside densely packed Particle Life clusters.
//
// cellStart() is written into whichever of the two scan ping-pong buffers is
// NOT holding the final inclusive-scan result; encodeBuild() tracks that, so
// call cellStart() only after encodeBuild() for the current frame. The scan
// pass count is fixed per grid, so it is the same physical buffer every
// frame.

#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"

#include <cstdint>
#include <string>

namespace life {

struct SpatialHashDesc {
    uint32_t maxParticles = 0;
    uint32_t width = 0, height = 0; // world size in px (scene resolution)
    float cellSize = 24.0f;         // == interactionRadius (§9.3)
    std::string label;
};

class SpatialHashGrid {
public:
    SpatialHashGrid() = default;

    bool create(ResourcePool& pool, const SpatialHashDesc& desc);
    void destroy();

    // Encodes the full build for `count` particles read from `positions`
    // (float2 buffer, the interaction sim's read side).
    void encodeBuild(CommandGraph& graph, const std::string& labelPrefix,
                     BufferHandle positions, uint32_t count);

    BufferHandle cellStart() const { return cellStartResult_; } // uint × numCells (exclusive start)
    BufferHandle cellCount() const { return cellCount_; }       // uint × numCells
    BufferHandle sortedIndices() const { return sortedIndices_; } // uint × maxParticles

    uint32_t cellsX() const { return cellsX_; }
    uint32_t cellsY() const { return cellsY_; }
    uint32_t numCells() const { return numCells_; }
    float cellSize() const { return desc_.cellSize; }

private:
    ResourcePool* pool_ = nullptr;
    SpatialHashDesc desc_;
    uint32_t cellsX_ = 0;
    uint32_t cellsY_ = 0;
    uint32_t numCells_ = 0;

    BufferHandle cellOf_;         // uint × maxParticles
    BufferHandle cellCount_;      // uint × numCells (atomic)
    BufferHandle scanA_, scanB_;  // uint × numCells (scan ping-pong; one ends
                                  // up holding cellStart, see header comment)
    BufferHandle cellCursor_;     // uint × numCells (atomic scatter cursors)
    BufferHandle sortedIndices_;  // uint × maxParticles
    BufferHandle cellStartResult_; // scanA_ or scanB_, set by encodeBuild()
};

} // namespace life
