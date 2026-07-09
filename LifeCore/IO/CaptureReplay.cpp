// LifeCore/IO/CaptureReplay.cpp
#include "LifeCore/IO/CaptureReplay.h"

#include <nlohmann/json.hpp>

namespace life {

namespace {

nlohmann::json envelopeToJson(const ChannelEnvelope& e) {
    return {e.raw, e.smoothed, e.peak, e.trigger, e.hold};
}

void envelopeFromJson(const nlohmann::json& j, ChannelEnvelope& e) {
    if (!j.is_array() || j.size() < 5) return;
    e.raw = j[0].get<float>();
    e.smoothed = j[1].get<float>();
    e.peak = j[2].get<float>();
    e.trigger = j[3].get<float>();
    e.hold = j[4].get<float>();
}

} // namespace

bool CaptureWriter::open(const std::string& path, std::string& outError) {
    file_.open(path, std::ios::trunc);
    if (!file_) {
        outError = "cannot open capture file for writing: " + path;
        return false;
    }
    frames_ = 0;
    return true;
}

void CaptureWriter::writeFrame(const AudioFeatureState& s) {
    if (!file_.is_open()) return;
    nlohmann::json j;
    j["f"] = s.frameIndex;
    j["t"] = s.time;
    j["dt"] = s.deltaTime;
    j["kick"] = envelopeToJson(s.kickEnv);
    j["snare"] = envelopeToJson(s.snareEnv);
    j["hihat"] = envelopeToJson(s.hihatEnv);
    j["perc"] = envelopeToJson(s.percEnv);
    j["beat"] = envelopeToJson(s.beatEnv);
    j["drv"] = {s.rms, s.low, s.mid, s.high, s.centroid, s.flux};
    j["fft"] = std::vector<float>(s.fft, s.fft + kFFTBins);
    file_ << j.dump() << "\n";
    frames_++;
}

void CaptureWriter::close() {
    if (file_.is_open()) file_.close();
}

bool CaptureReader::load(const std::string& path, std::string& outError) {
    std::ifstream f(path);
    if (!f) {
        outError = "cannot open capture file: " + path;
        return false;
    }
    frames_.clear();
    std::string line;
    size_t lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        if (line.empty()) continue;
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (const std::exception& e) {
            outError = "capture parse error at line " + std::to_string(lineNo) + ": " +
                       e.what();
            return false;
        }
        AudioFeatureState s;
        s.frameIndex = j.value("f", uint32_t(frames_.size()));
        s.time = j.value("t", 0.0f);
        s.deltaTime = j.value("dt", 1.0f / 60.0f);
        if (j.contains("kick")) envelopeFromJson(j["kick"], s.kickEnv);
        if (j.contains("snare")) envelopeFromJson(j["snare"], s.snareEnv);
        if (j.contains("hihat")) envelopeFromJson(j["hihat"], s.hihatEnv);
        if (j.contains("perc")) envelopeFromJson(j["perc"], s.percEnv);
        if (j.contains("beat")) envelopeFromJson(j["beat"], s.beatEnv);
        s.kick = s.kickEnv.smoothed;
        s.snare = s.snareEnv.smoothed;
        s.hihat = s.hihatEnv.smoothed;
        s.perc = s.percEnv.smoothed;
        s.beat = s.beatEnv.smoothed;
        if (j.contains("drv") && j["drv"].is_array() && j["drv"].size() >= 6) {
            s.rms = j["drv"][0].get<float>();
            s.low = j["drv"][1].get<float>();
            s.mid = j["drv"][2].get<float>();
            s.high = j["drv"][3].get<float>();
            s.centroid = j["drv"][4].get<float>();
            s.flux = j["drv"][5].get<float>();
        }
        if (j.contains("fft") && j["fft"].is_array()) {
            int n = std::min<int>(kFFTBins, int(j["fft"].size()));
            for (int i = 0; i < n; ++i) s.fft[i] = j["fft"][i].get<float>();
        }
        frames_.push_back(std::move(s));
    }
    if (frames_.empty()) {
        outError = "capture file has no frames: " + path;
        return false;
    }
    return true;
}

const AudioFeatureState& CaptureReader::frame(size_t frameIndex) const {
    if (frames_.empty()) return empty_;
    if (frameIndex >= frames_.size()) return frames_.back();
    return frames_[frameIndex];
}

} // namespace life
