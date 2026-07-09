// LifeCore/Audio/OSCReceiver.cpp
#include "LifeCore/Audio/OSCReceiver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace life {

namespace {

// OSC strings/blobs are padded to 4-byte boundaries.
size_t paddedSize(size_t n) { return (n + 3) & ~size_t(3); }

uint32_t readBigU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}

float readBigFloat(const uint8_t* p) {
    uint32_t u = readBigU32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

} // namespace

OSCReceiver::~OSCReceiver() { stop(); }

bool OSCReceiver::start(uint16_t port, MessageCallback callback, std::string& outError) {
    stop();
    callback_ = std::move(callback);
    port_ = port;

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        outError = "socket() failed: " + std::string(strerror(errno));
        return false;
    }
    int reuse = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // Generous receive buffer: /fft at 60 Hz is ~560B per message.
    int rcvbuf = 1 << 20;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        outError = "bind(" + std::to_string(port) + ") failed: " + strerror(errno);
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    // Blocking socket with a timeout so the thread can notice stop().
    timeval tv{};
    tv.tv_usec = 100 * 1000; // 100ms
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    running_.store(true);
    thread_ = std::thread([this] { receiveLoop(); });
    return true;
}

void OSCReceiver::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
    }
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

OSCStats OSCReceiver::stats() const {
    return {packets_.load(), messages_.load(), errors_.load()};
}

void OSCReceiver::receiveLoop() {
    std::vector<uint8_t> buffer(65536);
    while (running_.load()) {
        ssize_t n = ::recv(socket_, buffer.data(), buffer.size(), 0);
        if (n <= 0) continue; // timeout or transient error
        packets_.fetch_add(1);
        parsePacket(buffer.data(), size_t(n));
    }
}

void OSCReceiver::parsePacket(const uint8_t* data, size_t size) {
    if (size >= 8 && std::memcmp(data, "#bundle", 8) == 0) {
        parseBundle(data, size);
    } else {
        parseMessage(data, size);
    }
}

void OSCReceiver::parseBundle(const uint8_t* data, size_t size) {
    // "#bundle\0" (8) + timetag (8) + [int32 size + element]*
    if (size < 16) { errors_.fetch_add(1); return; }
    size_t offset = 16;
    while (offset + 4 <= size) {
        uint32_t elemSize = readBigU32(data + offset);
        offset += 4;
        if (elemSize == 0 || offset + elemSize > size) { errors_.fetch_add(1); return; }
        parsePacket(data + offset, elemSize); // elements may be nested bundles
        offset += elemSize;
    }
}

void OSCReceiver::parseMessage(const uint8_t* data, size_t size) {
    // Address pattern
    size_t addrLen = strnlen(reinterpret_cast<const char*>(data), size);
    if (addrLen == 0 || addrLen == size || data[0] != '/') {
        errors_.fetch_add(1);
        return;
    }
    std::string address(reinterpret_cast<const char*>(data), addrLen);
    size_t offset = paddedSize(addrLen + 1);
    if (offset >= size) { errors_.fetch_add(1); return; }

    // Type tag string (",fff...")
    if (data[offset] != ',') { errors_.fetch_add(1); return; }
    size_t tagLen = strnlen(reinterpret_cast<const char*>(data + offset), size - offset);
    std::string tags(reinterpret_cast<const char*>(data + offset + 1), tagLen - 1);
    offset += paddedSize(tagLen + 1);

    scratch_.clear();
    for (char t : tags) {
        switch (t) {
            case 'f': {
                if (offset + 4 > size) { errors_.fetch_add(1); return; }
                scratch_.push_back(readBigFloat(data + offset));
                offset += 4;
                break;
            }
            case 'i': {
                if (offset + 4 > size) { errors_.fetch_add(1); return; }
                scratch_.push_back(float(int32_t(readBigU32(data + offset))));
                offset += 4;
                break;
            }
            case 'd': {
                if (offset + 8 > size) { errors_.fetch_add(1); return; }
                uint64_t u = (uint64_t(readBigU32(data + offset)) << 32) |
                             readBigU32(data + offset + 4);
                double d;
                std::memcpy(&d, &u, 8);
                scratch_.push_back(float(d));
                offset += 8;
                break;
            }
            case 'b': {
                if (offset + 4 > size) { errors_.fetch_add(1); return; }
                uint32_t blobSize = readBigU32(data + offset);
                offset += 4;
                if (offset + blobSize > size) { errors_.fetch_add(1); return; }
                // Interpret as packed little-endian float32 array (host layout
                // used by SC / Max senders). Big-endian senders should use the
                // plain float-argument form instead (design doc §2.1 方式A).
                size_t count = blobSize / 4;
                for (size_t i = 0; i < count; ++i) {
                    float f;
                    std::memcpy(&f, data + offset + i * 4, 4);
                    scratch_.push_back(f);
                }
                offset += paddedSize(blobSize);
                break;
            }
            case 'T': scratch_.push_back(1.0f); break;
            case 'F': scratch_.push_back(0.0f); break;
            case 's': { // skip strings
                size_t sl = strnlen(reinterpret_cast<const char*>(data + offset),
                                    size - offset);
                offset += paddedSize(sl + 1);
                break;
            }
            default:
                // Unknown tag: bail out rather than misalign.
                errors_.fetch_add(1);
                return;
        }
    }

    messages_.fetch_add(1);
    if (callback_)
        callback_(address, scratch_.data(), int(scratch_.size()));
}

} // namespace life
