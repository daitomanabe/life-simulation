#pragma once
// LifeCore/IO/NpyWriter.h
// Hand-rolled NumPy .npy v1.0 writer (float32, C order) — no dependency on
// numpy/cnpy. Used by SimulationModule::dumpState() implementations to hand
// GPU state to the Python print renderer (brochure/render_view.py).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace life {

// count must equal the product of shape. Row-major (C order), matching
// numpy's default and 'fortran_order': False in the header.
bool writeNPYFloat32(const std::string& path, const float* data, size_t count,
                     const std::vector<uint32_t>& shape, std::string& outError);

// arm64 has native half support via __fp16 (same trick as ImageWriter.mm /
// LifeOfflineRender's RawLumaWriter — works in plain .cpp, no ObjC needed).
inline float half2float(uint16_t h) {
    __fp16 v;
    std::memcpy(&v, &h, 2);
    return float(v);
}

} // namespace life
