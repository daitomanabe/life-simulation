#pragma once
// LifeCore/IO/ImageWriter.h
// PNG via ImageIO (zero extra dependencies on macOS) and EXR via TinyEXR.
// Input is always RGBA16F pixel data — the suite's unified output format.

#include <cstdint>
#include <string>

namespace life {

// pixels: RGBA float16, tightly packed, width*height*4 halfs.
// PNG applies exposure then linear→sRGB encode into 8-bit.
bool writePNGFromHalfRGBA(const std::string& path, const uint16_t* pixels,
                          uint32_t width, uint32_t height, float exposure,
                          std::string& outError);

// EXR keeps linear values, stored as half (fp16).
bool writeEXRFromHalfRGBA(const std::string& path, const uint16_t* pixels,
                          uint32_t width, uint32_t height, std::string& outError);

} // namespace life
