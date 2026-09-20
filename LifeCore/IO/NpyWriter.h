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

// Inverse of writeNPYFloat32() — used by SimulationModule::loadState()
// implementations (design point: LifeRealtime --load-state). `expectedShape`
// is the shape the CALLER needs (derived from its own live buffer/texture
// size, e.g. {height, width} or {agentCount, 2}) — it is REQUIRED to be
// non-empty and is checked against the file's own declared shape before a
// single byte of payload is read. This is what makes a truncated or
// corrupted .npy (tests/golden_md5-style fuzzing, a snapshot cut off by a
// power loss) fail safely: outData is only ever sized to the product of
// `expectedShape` (never to whatever a garbled header claims), and a short
// read on the payload is reported as an error rather than silently zero-
// filled. Returns false with outError set on any problem — bad magic,
// truncated header, unsupported dtype, shape mismatch, or truncated data.
bool readNPYFloat32(const std::string& path, const std::vector<uint32_t>& expectedShape,
                    std::vector<float>& outData, std::string& outError);

// arm64 has native half support via __fp16 (same trick as ImageWriter.mm /
// LifeOfflineRender's RawLumaWriter — works in plain .cpp, no ObjC needed).
inline float half2float(uint16_t h) {
    __fp16 v;
    std::memcpy(&v, &h, 2);
    return float(v);
}

inline uint16_t float2half(float f) {
    __fp16 v = (__fp16)f;
    uint16_t h;
    std::memcpy(&h, &v, 2);
    return h;
}

} // namespace life
