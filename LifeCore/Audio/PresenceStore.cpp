// LifeCore/Audio/PresenceStore.cpp
#include "LifeCore/Audio/PresenceStore.h"

#include <cstdio>

namespace life {

void PresenceStore::pushPoint(uint32_t id, float x, float y, float strength) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingPushes_.push_back({id, x, y, strength});
    pendingActivity_ = true;
}

void PresenceStore::pushClear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingClear_ = true;
    pendingActivity_ = true;
}

void PresenceStore::update(float dt, PresenceState& out) {
    std::vector<PendingPoint> pushes;
    bool clear = false;
    bool activity = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pushes.swap(pendingPushes_);
        clear = pendingClear_;
        pendingClear_ = false;
        activity = pendingActivity_;
        pendingActivity_ = false;
    }

    // Age every currently tracked point by dt BEFORE applying this frame's
    // pushes, so a point refreshed this frame ends the frame at age 0 (not
    // age dt) — same "reset on refresh, else accumulate" shape as
    // FeatureSmoother::update's idleTime_.
    for (auto& s : table_)
        if (s.used) s.age += dt;

    // If both a clear and fresh points arrived within the same sim frame
    // (rare at 60fps), clear is applied first, then the frame's points are
    // (re)added — the common "clear before repopulating" case. Sub-frame
    // OSC arrival order between the two isn't preserved; not worth a queue
    // for a once-per-16ms merge.
    if (clear) {
        for (auto& s : table_) s.used = false;
    }

    for (const auto& p : pushes) {
        Slot* slot = nullptr;
        for (auto& s : table_) {
            if (s.used && s.id == p.id) {
                slot = &s;
                break;
            }
        }
        if (!slot) {
            for (auto& s : table_) {
                if (!s.used) {
                    slot = &s;
                    break;
                }
            }
        }
        if (!slot) {
            if (!warnedFull_) {
                fprintf(stderr,
                        "[life] /presence: store full (%d points tracked), dropping id=%u\n",
                        kMaxPresencePoints, p.id);
                warnedFull_ = true;
            }
            continue;
        }
        slot->id = p.id;
        slot->x = p.x;
        slot->y = p.y;
        slot->strength = p.strength;
        slot->age = 0.0f;
        slot->used = true;
    }

    // Not full any more (something was dropped, or freed by /presence/clear) —
    // let the warning fire again next time capacity is actually exceeded.
    uint32_t usedCount = 0;
    for (auto& s : table_)
        if (s.used) usedCount++;
    if (usedCount < kMaxPresencePoints) warnedFull_ = false;

    secondsSinceMessage_ = activity ? 0.0f : secondsSinceMessage_ + dt;

    out.count = 0;
    for (auto& s : table_) {
        if (!s.used) continue;
        out.points[out.count] = {s.id, s.x, s.y, s.strength, s.age};
        out.count++;
    }
    out.secondsSinceMessage = secondsSinceMessage_;
}

} // namespace life
