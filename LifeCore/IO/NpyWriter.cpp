// LifeCore/IO/NpyWriter.cpp
#include "LifeCore/IO/NpyWriter.h"

#include <cstdio>

namespace life {

bool writeNPYFloat32(const std::string& path, const float* data, size_t count,
                     const std::vector<uint32_t>& shape, std::string& outError) {
    size_t expected = 1;
    for (uint32_t s : shape) expected *= s;
    if (expected != count) {
        outError = "writeNPYFloat32: shape/count mismatch for " + path;
        return false;
    }

    // Header dict, numpy v1.0 format: magic(6) + version(2) + headerLen(2) +
    // dict string, padded with spaces so the WHOLE preamble is a multiple of
    // 64 bytes, ending in '\n'.
    std::string shapeStr = "(";
    for (size_t i = 0; i < shape.size(); ++i) {
        shapeStr += std::to_string(shape[i]);
        if (shape.size() == 1 || i + 1 < shape.size()) shapeStr += ", ";
    }
    shapeStr += ")";

    std::string dict = "{'descr': '<f4', 'fortran_order': False, 'shape': " + shapeStr + ", }";
    const size_t preambleFixed = 6 + 2 + 2; // magic + version + header-length field
    size_t total = preambleFixed + dict.size() + 1; // +1 for trailing '\n'
    size_t pad = (64 - (total % 64)) % 64;
    dict.append(pad, ' ');
    dict.push_back('\n');

    if (dict.size() > 0xFFFFu) {
        outError = "writeNPYFloat32: header too large for " + path;
        return false;
    }
    uint16_t headerLen = uint16_t(dict.size());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        outError = "cannot open " + path + " for writing";
        return false;
    }
    const unsigned char magic[6] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    const unsigned char version[2] = {1, 0};
    unsigned char lenBytes[2] = {uint8_t(headerLen & 0xFF), uint8_t((headerLen >> 8) & 0xFF)};

    bool ok = std::fwrite(magic, 1, 6, f) == 6 && std::fwrite(version, 1, 2, f) == 2 &&
              std::fwrite(lenBytes, 1, 2, f) == 2 &&
              std::fwrite(dict.data(), 1, dict.size(), f) == dict.size();
    if (ok && count > 0)
        ok = std::fwrite(data, sizeof(float), count, f) == count;
    std::fclose(f);
    if (!ok) {
        outError = "short write on " + path;
        return false;
    }
    return true;
}

} // namespace life
