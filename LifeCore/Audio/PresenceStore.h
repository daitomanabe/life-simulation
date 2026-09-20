#pragma once
// LifeCore/Audio/PresenceStore.h
// Thread-safe merge point for /presence OSC input (sensor-agnostic visitor
// tracking along the wall). Mirrors FeatureSmoother's push/update split:
// the OSC receive thread only ever appends to a pending buffer under a
// mutex; update(dt) — called once per sim frame from AudioInput::update,
// same as FeatureSmoother::update(dt) — drains it, merges by id into a
// fixed-capacity table, and ages every slot by dt. Never std::chrono: age
// and secondsSinceMessage are dt-accumulated so an offline render replaying
// recorded /presence input would reproduce byte-identically (same
// determinism contract as FeatureSmoother's idleTime_).
//
// This class only tracks points and their age/capacity — it never drops a
// point for being stale and never itself declares "no signal". That policy
// (holdSeconds / watchdogSeconds) belongs to the consuming module
// (PresenceField), which is configured per scene; this store's job is
// bookkeeping only.

#include "LifeCore/Audio/AudioFeatureState.h"

#include <array>
#include <mutex>
#include <vector>

namespace life {

class PresenceStore {
public:
    // OSC receive thread. x/y are expected already-normalized 0..1 (the
    // caller — AudioInput's /presence dispatch — wraps/clamps at the OSC
    // trust boundary before calling this).
    void pushPoint(uint32_t id, float x, float y, float strength);
    // OSC receive thread: /presence/clear — drop every tracked point.
    void pushClear();

    // Sim thread, once per frame: merge pending pushes, age every tracked
    // point by dt, and fill `out` with the current raw table.
    void update(float dt, PresenceState& out);

private:
    struct PendingPoint {
        uint32_t id;
        float x, y, strength;
    };

    std::mutex mutex_;
    std::vector<PendingPoint> pendingPushes_; // OSC thread appends; sim thread drains
    bool pendingClear_ = false;
    bool pendingActivity_ = false; // a push or clear arrived since the last update()

    struct Slot {
        uint32_t id = 0;
        float x = 0.0f, y = 0.0f, strength = 1.0f, age = 0.0f;
        bool used = false;
    };
    std::array<Slot, kMaxPresencePoints> table_{};
    float secondsSinceMessage_ = 1.0e9f;
    bool warnedFull_ = false; // rate-limits the "store full" log to one line per overflow episode
};

} // namespace life
