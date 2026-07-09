#pragma once
// LifeCore/Audio/OSCReceiver.h
// Minimal, dependency-free OSC 1.0 receiver (UDP). Supports messages and
// nested bundles; argument types f, i, d, b. Kept deliberately small — this
// is an input bus for /kick /snare /hihat /perc /beat /fft only (design doc
// §2.1), not a general OSC framework. The parser is API-compatible in spirit
// with oscpack so it could be swapped later without touching callers.
//
// Threading: start() spawns a receive thread that pushes into the callback
// (typically FeatureSmoother::pushChannel / pushFFT, which are thread-safe).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace life {

struct OSCStats {
    uint64_t packetsReceived = 0;
    uint64_t messagesParsed = 0;
    uint64_t parseErrors = 0;
};

class OSCReceiver {
public:
    // address: e.g. "/kick"; floats: parsed float arguments (blob args of
    // size 4*N are decoded as N little-endian floats — the common layout for
    // /fft blobs from SuperCollider / Max on little-endian hosts).
    using MessageCallback =
        std::function<void(const std::string& address, const float* values, int count)>;

    OSCReceiver() = default;
    ~OSCReceiver();

    OSCReceiver(const OSCReceiver&) = delete;
    OSCReceiver& operator=(const OSCReceiver&) = delete;

    bool start(uint16_t port, MessageCallback callback, std::string& outError);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }

    OSCStats stats() const;

private:
    void receiveLoop();
    void parsePacket(const uint8_t* data, size_t size);
    void parseBundle(const uint8_t* data, size_t size);
    void parseMessage(const uint8_t* data, size_t size);

    int socket_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    MessageCallback callback_;

    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> messages_{0};
    std::atomic<uint64_t> errors_{0};

    std::vector<float> scratch_; // reused arg buffer (receive thread only)
};

} // namespace life
