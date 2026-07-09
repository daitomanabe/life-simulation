// Shaders/Spatial/SpatialHash.metal
// 2D uniform-grid spatial hash build (design doc §9, Phase 4). Counting sort:
// count per cell → Hillis-Steele inclusive scan (C++ encodes one pass per
// stride, ping-ponging scanA/scanB) → exclusive starts + cursors → atomic
// scatter → per-cell sort. The atomic scatter's write order is
// GPU-schedule dependent, so hashSortCells restores the canonical ascending
// particle-index order inside each cell — that makes every consumer's
// neighbor iteration order (and therefore its float accumulation order)
// deterministic for a given seed. Matches life::SpatialHashGrid.
//
// GridBuildParams mirrors the scalar-packed struct in
// LifeCore/Spatial/SpatialHashGrid.cpp (same field order).

struct GridBuildParams {
    uint particleCount;
    uint numCells;
    uint cellsX;
    uint cellsY;
    float cellSize;
    float worldW;
    float worldH;
    uint scanStride; // updated by the C++ side before each scan pass
};

kernel void hashClearCells(device atomic_uint* cellCount [[buffer(0)]],
                           constant GridBuildParams& p [[buffer(1)]],
                           uint id [[thread_position_in_grid]]) {
    if (id >= p.numCells) return;
    atomic_store_explicit(&cellCount[id], 0u, memory_order_relaxed);
}

kernel void hashCount(device const float2* positions [[buffer(0)]],
                      device uint* cellOf [[buffer(1)]],
                      device atomic_uint* cellCount [[buffer(2)]],
                      constant GridBuildParams& p [[buffer(3)]],
                      uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    // Wrap into [0,W)×[0,H) (toroidal world; fract handles negatives), then
    // clamp the cell coordinate so a float rounding to exactly W/H can never
    // index one past the last cell.
    float2 world = float2(p.worldW, p.worldH);
    float2 pos = fract(positions[id] / world) * world;
    uint cx = min(uint(pos.x / p.cellSize), p.cellsX - 1u);
    uint cy = min(uint(pos.y / p.cellSize), p.cellsY - 1u);
    uint cell = cy * p.cellsX + cx;

    cellOf[id] = cell;
    atomic_fetch_add_explicit(&cellCount[cell], 1u, memory_order_relaxed);
}

// One Hillis-Steele step: dst[i] = src[i] + src[i - stride]. The C++ side
// encodes ceil(log2(numCells)) of these with stride = 1,2,4,... ping-ponging
// scanA/scanB (first src is cellCount); each pass runs in its own compute
// encoder, so the previous stride's writes are visible.
kernel void hashScanInclusive(device const uint* src [[buffer(0)]],
                              device uint* dst [[buffer(1)]],
                              constant GridBuildParams& p [[buffer(2)]],
                              uint id [[thread_position_in_grid]]) {
    if (id >= p.numCells) return;
    uint v = src[id];
    if (id >= p.scanStride) v += src[id - p.scanStride];
    dst[id] = v;
}

// inclusive → exclusive starts; cursors start at the cell start so
// hashScatter can hand out slots with atomic_fetch_add.
kernel void hashFinalize(device const uint* inclusive [[buffer(0)]],
                         device const uint* cellCount [[buffer(1)]],
                         device uint* cellStart [[buffer(2)]],
                         device atomic_uint* cellCursor [[buffer(3)]],
                         constant GridBuildParams& p [[buffer(4)]],
                         uint id [[thread_position_in_grid]]) {
    if (id >= p.numCells) return;
    uint start = inclusive[id] - cellCount[id];
    cellStart[id] = start;
    atomic_store_explicit(&cellCursor[id], start, memory_order_relaxed);
}

kernel void hashScatter(device const uint* cellOf [[buffer(0)]],
                        device atomic_uint* cellCursor [[buffer(1)]],
                        device uint* sortedIndices [[buffer(2)]],
                        constant GridBuildParams& p [[buffer(3)]],
                        uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;
    uint slot = atomic_fetch_add_explicit(&cellCursor[cellOf[id]], 1u,
                                          memory_order_relaxed);
    sortedIndices[slot] = id;
}

// Determinism keystone: sort each cell's slice of sortedIndices into
// ascending particle-index order, erasing the nondeterministic atomic
// scatter order. Any full comparison sort yields the same canonical result,
// so every cell is sorted unconditionally — the spec's ">256 → skip" escape
// hatch is NOT taken, because Particle Life clusters routinely pack
// hundreds-to-thousands of particles into one interaction-radius cell and a
// skipped cell breaks bit-exact repeatability (completion condition 3).
// To keep that affordable, the plain insertion sort is run over a Knuth gap
// sequence (Shell sort: the final gap-1 pass IS the insertion sort), which
// bounds the single-thread worst case at ~O(n^1.5) instead of O(n²).
kernel void hashSortCells(device const uint* cellStart [[buffer(0)]],
                          device const uint* cellCount [[buffer(1)]],
                          device uint* sortedIndices [[buffer(2)]],
                          constant GridBuildParams& p [[buffer(3)]],
                          uint id [[thread_position_in_grid]]) {
    if (id >= p.numCells) return;
    uint start = cellStart[id];
    uint n = cellCount[id];
    if (n < 2u) return;

    uint h = 1u;
    while (h < n / 3u) h = h * 3u + 1u; // Knuth: 1, 4, 13, 40, 121, ...
    for (; h >= 1u; h = (h - 1u) / 3u) {
        for (uint a = h; a < n; ++a) {
            uint v = sortedIndices[start + a];
            uint b = a;
            while (b >= h && sortedIndices[start + b - h] > v) {
                sortedIndices[start + b] = sortedIndices[start + b - h];
                b -= h;
            }
            sortedIndices[start + b] = v;
        }
    }
}
