// LifeCore/Spatial/SpatialHashGrid.cpp
#include "LifeCore/Spatial/SpatialHashGrid.h"

#include <cmath>
#include <string>

namespace life {

namespace {
// Mirrors GridBuildParams in Shaders/Spatial/SpatialHash.metal (scalar-packed,
// same field order).
struct GridBuildParams {
    uint32_t particleCount;
    uint32_t numCells;
    uint32_t cellsX;
    uint32_t cellsY;
    float cellSize;
    float worldW;
    float worldH;
    uint32_t scanStride;
};
static_assert(sizeof(GridBuildParams) == 8 * 4,
              "GridBuildParams layout must stay scalar-packed to match MSL");
} // namespace

bool SpatialHashGrid::create(ResourcePool& pool, const SpatialHashDesc& desc) {
    if (desc.maxParticles == 0 || desc.width == 0 || desc.height == 0 ||
        desc.cellSize <= 0.0f)
        return false;

    pool_ = &pool;
    desc_ = desc;
    cellsX_ = uint32_t(std::ceil(float(desc.width) / desc.cellSize));
    cellsY_ = uint32_t(std::ceil(float(desc.height) / desc.cellSize));
    numCells_ = cellsX_ * cellsY_;

    BufferDesc bd;
    bd.storage = StorageMode::GPUPrivate;

    bd.size = size_t(desc.maxParticles) * 4; // uint
    bd.label = desc.label + ".cellOf";
    cellOf_ = pool.createBuffer(bd);
    bd.label = desc.label + ".sortedIndices";
    sortedIndices_ = pool.createBuffer(bd);

    bd.size = size_t(numCells_) * 4; // uint
    bd.label = desc.label + ".cellCount";
    cellCount_ = pool.createBuffer(bd);
    bd.label = desc.label + ".scanA";
    scanA_ = pool.createBuffer(bd);
    bd.label = desc.label + ".scanB";
    scanB_ = pool.createBuffer(bd);
    bd.label = desc.label + ".cellCursor";
    cellCursor_ = pool.createBuffer(bd);

    cellStartResult_ = {};
    return cellOf_.valid() && sortedIndices_.valid() && cellCount_.valid() &&
           scanA_.valid() && scanB_.valid() && cellCursor_.valid();
}

void SpatialHashGrid::destroy() {
    if (!pool_) return;
    pool_->release(cellOf_);
    pool_->release(cellCount_);
    pool_->release(scanA_);
    pool_->release(scanB_);
    pool_->release(cellCursor_);
    pool_->release(sortedIndices_);
    cellOf_ = cellCount_ = scanA_ = scanB_ = cellCursor_ = sortedIndices_ = {};
    cellStartResult_ = {};
}

void SpatialHashGrid::encodeBuild(CommandGraph& graph, const std::string& labelPrefix,
                                  BufferHandle positions, uint32_t count) {
    GridBuildParams p{};
    p.particleCount = count;
    p.numCells = numCells_;
    p.cellsX = cellsX_;
    p.cellsY = cellsY_;
    p.cellSize = desc_.cellSize;
    p.worldW = float(desc_.width);
    p.worldH = float(desc_.height);
    p.scanStride = 0;

    graph.pass(labelPrefix + ".hashClearCells")
        .pipeline("hashClearCells")
        .buffer(0, cellCount_)
        .uniforms(1, p)
        .dispatch1D(numCells_);

    graph.pass(labelPrefix + ".hashCount")
        .pipeline("hashCount")
        .buffer(0, positions)
        .buffer(1, cellOf_)
        .buffer(2, cellCount_)
        .uniforms(3, p)
        .dispatch1D(count);

    // Hillis-Steele inclusive scan of cellCount, one pass per stride,
    // ping-ponging scanA/scanB. `src` tracks where the latest partial result
    // lives; for numCells == 1 no pass runs and cellCount itself is already
    // the inclusive scan.
    BufferHandle src = cellCount_;
    bool dstIsA = true;
    for (uint32_t stride = 1; stride < numCells_; stride <<= 1) {
        BufferHandle dst = dstIsA ? scanA_ : scanB_;
        p.scanStride = stride;
        graph.pass(labelPrefix + ".hashScan.s" + std::to_string(stride))
            .pipeline("hashScanInclusive")
            .buffer(0, src)
            .buffer(1, dst)
            .uniforms(2, p)
            .dispatch1D(numCells_);
        src = dst;
        dstIsA = !dstIsA;
    }
    BufferHandle inclusive = src;
    // cellStart goes into the scan buffer that is NOT holding the final
    // inclusive result. The pass count is fixed per grid, so this is the same
    // physical buffer every build.
    BufferHandle startBuf = dstIsA ? scanA_ : scanB_;

    graph.pass(labelPrefix + ".hashFinalize")
        .pipeline("hashFinalize")
        .buffer(0, inclusive)
        .buffer(1, cellCount_)
        .buffer(2, startBuf)
        .buffer(3, cellCursor_)
        .uniforms(4, p)
        .dispatch1D(numCells_);
    cellStartResult_ = startBuf;

    graph.pass(labelPrefix + ".hashScatter")
        .pipeline("hashScatter")
        .buffer(0, cellOf_)
        .buffer(1, cellCursor_)
        .buffer(2, sortedIndices_)
        .uniforms(3, p)
        .dispatch1D(count);

    // Determinism keystone — do not skip (spec §1): canonical ascending-index
    // order inside each cell makes neighbor iteration order deterministic.
    graph.pass(labelPrefix + ".hashSortCells")
        .pipeline("hashSortCells")
        .buffer(0, startBuf)
        .buffer(1, cellCount_)
        .buffer(2, sortedIndices_)
        .uniforms(3, p)
        .dispatch1D(numCells_);
}

} // namespace life
