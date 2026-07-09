#pragma once
// LifeCore/IO/CaptureReplay.h
// Records / replays AudioFeatureState as JSONL (one frame per line). This is
// what makes a live moment reproducible offline (design doc §4.6): capture in
// LifeRealtime with --record, replay in LifeOfflineRender with --audio.

#include "LifeCore/Audio/AudioFeatureState.h"

#include <fstream>
#include <string>
#include <vector>

namespace life {

class CaptureWriter {
public:
    bool open(const std::string& path, std::string& outError);
    void writeFrame(const AudioFeatureState& state);
    void close();
    bool isOpen() const { return file_.is_open(); }
    uint64_t framesWritten() const { return frames_; }

private:
    std::ofstream file_;
    uint64_t frames_ = 0;
};

class CaptureReader {
public:
    bool load(const std::string& path, std::string& outError);
    size_t frameCount() const { return frames_.size(); }

    // Returns the recorded state for frameIndex, clamped to the last frame.
    // time/deltaTime/frameIndex are overwritten by the runner's own clock so
    // replay at a different fps still behaves deterministically.
    const AudioFeatureState& frame(size_t frameIndex) const;

private:
    std::vector<AudioFeatureState> frames_;
    AudioFeatureState empty_;
};

} // namespace life
