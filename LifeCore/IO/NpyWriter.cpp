// LifeCore/IO/NpyWriter.cpp
#include "LifeCore/IO/NpyWriter.h"

#include <cstdio>

namespace life {

namespace {
// Textual parse of "'shape': (12, 34, )" / "(12,)" — the only forms our own
// writeNPYFloat32() ever produces. Returns false on anything that doesn't
// look like that (defensive against a corrupted/foreign header, not a
// general numpy header parser).
bool parseShapeTuple(const std::string& dict, std::vector<uint32_t>& outShape) {
    auto key = dict.find("'shape':");
    if (key == std::string::npos) return false;
    auto open = dict.find('(', key);
    auto close = dict.find(')', open == std::string::npos ? key : open);
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    std::string inner = dict.substr(open + 1, close - open - 1);
    outShape.clear();
    size_t p = 0;
    while (p < inner.size()) {
        size_t c = inner.find(',', p);
        std::string tok = inner.substr(p, c == std::string::npos ? std::string::npos : c - p);
        // trim whitespace
        size_t b = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (b != std::string::npos) {
            tok = tok.substr(b, e - b + 1);
            if (!tok.empty()) {
                for (char ch : tok) if (ch < '0' || ch > '9') return false;
                outShape.push_back(uint32_t(std::stoul(tok)));
            }
        }
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return true;
}
} // namespace

bool readNPYFloat32(const std::string& path, const std::vector<uint32_t>& expectedShape,
                    std::vector<float>& outData, std::string& outError) {
    outData.clear();
    if (expectedShape.empty()) {
        outError = "readNPYFloat32: expectedShape must not be empty (" + path + ")";
        return false;
    }
    size_t expectedCount = 1;
    for (uint32_t s : expectedShape) expectedCount *= s;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        outError = "cannot open " + path + " for reading";
        return false;
    }

    unsigned char magic[6];
    unsigned char version[2];
    unsigned char lenBytes[2];
    bool okHeader =
        std::fread(magic, 1, 6, f) == 6 &&
        magic[0] == 0x93 && magic[1] == 'N' && magic[2] == 'U' && magic[3] == 'M' &&
        magic[4] == 'P' && magic[5] == 'Y' &&
        std::fread(version, 1, 2, f) == 2 && version[0] == 1 &&
        std::fread(lenBytes, 1, 2, f) == 2;
    if (!okHeader) {
        std::fclose(f);
        outError = "corrupt or truncated npy header: " + path;
        return false;
    }
    uint16_t headerLen = uint16_t(lenBytes[0]) | (uint16_t(lenBytes[1]) << 8);
    std::string dict(headerLen, '\0');
    if (headerLen == 0 || std::fread(dict.data(), 1, headerLen, f) != headerLen) {
        std::fclose(f);
        outError = "truncated npy header dict: " + path;
        return false;
    }
    if (dict.find("'<f4'") == std::string::npos) {
        std::fclose(f);
        outError = "unsupported npy dtype (expected <f4): " + path;
        return false;
    }
    std::vector<uint32_t> fileShape;
    if (!parseShapeTuple(dict, fileShape)) {
        std::fclose(f);
        outError = "cannot parse npy shape: " + path;
        return false;
    }
    if (fileShape != expectedShape) {
        std::fclose(f);
        outError = "npy shape mismatch in " + path + ": expected (";
        for (size_t i = 0; i < expectedShape.size(); ++i)
            outError += (i ? ", " : "") + std::to_string(expectedShape[i]);
        outError += "), file has (";
        for (size_t i = 0; i < fileShape.size(); ++i)
            outError += (i ? ", " : "") + std::to_string(fileShape[i]);
        outError += ")";
        return false;
    }

    // Bounded by expectedCount (derived from the CALLER's own live buffer
    // size, never from the file), so a garbled header can never trigger an
    // oversized allocation here.
    outData.resize(expectedCount);
    size_t got = expectedCount > 0 ? std::fread(outData.data(), sizeof(float), expectedCount, f) : 0;
    std::fclose(f);
    if (got != expectedCount) {
        outData.clear();
        outError = "truncated npy data in " + path + ": expected " +
                   std::to_string(expectedCount) + " float32 values, got " + std::to_string(got);
        return false;
    }
    return true;
}

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
