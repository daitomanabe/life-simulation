// Apps/LifePresetLab/main.cpp
// Phase 7 (docs/specs/phase7_presetlab.md): parameter/seed sweep runner.
// Explores a scene's parameter space by re-instantiating SceneRunner once per
// variant (fresh seed each time — design doc-style determinism, §17), grading
// each variant with a cheap "activity" luma metric so a VJ can skim a
// self-contained HTML gallery for "alive" parameter regions before a show.
//
// Parameters are applied by mutating the parsed Scene's module params JSON
// directly (NOT via ParameterBus) so setup()-time-only params (radius,
// initCoverage, agentCount, ...) are sweepable too — see §2 of the spec.
//
// Activity is read from FrameRecorder::halfPixels() (raw RGBA16F), never by
// re-reading an exported PNG — same half->float cast as ImageWriter.mm
// (arm64 native __fp16). LifePresetLab owns its own FrameRecorder and drives
// its own beginFrame/encodeReadback/endFrame bracket (SceneRunner::step()'s
// own internal recorder is private, so this is the only way to reach raw
// pixels through SceneRunner's public API without touching SceneRunner.{h,cpp}
// — this phase's only allowed existing-file edit is CMakeLists.txt).

#include "Apps/Common/AppCommon.h"
#include "LifeCore/IO/CaptureReplay.h"
#include "LifeCore/IO/FrameRecorder.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#include <CLI11/CLI11.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace life;
using nlohmann::json;

namespace {

// ---- half -> float -------------------------------------------------------
// arm64 has native half support via __fp16 (mirrors LifeCore/IO/ImageWriter.mm;
// duplicated here rather than shared because that file is Objective-C++ and
// this app is plain C++, and this phase may not touch existing files besides
// CMakeLists.txt).
inline float halfToFloat(uint16_t h) {
    __fp16 v;
    std::memcpy(&v, &h, 2);
    return float(v);
}

// ---- activity metric (spec §3) -------------------------------------------

struct LumaStats {
    double mean = 0.0;
    double stddev = 0.0;
};

LumaStats computeLumaStats(const std::vector<uint16_t>& px, uint32_t w, uint32_t h) {
    LumaStats stats;
    const size_t n = size_t(w) * size_t(h);
    if (n == 0 || px.size() < n * 4) return stats;
    double sum = 0.0, sumSq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const float r = halfToFloat(px[i * 4 + 0]);
        const float g = halfToFloat(px[i * 4 + 1]);
        const float b = halfToFloat(px[i * 4 + 2]);
        const double l = 0.2126 * double(r) + 0.7152 * double(g) + 0.0722 * double(b);
        sum += l;
        sumSq += l * l;
    }
    const double mean = sum / double(n);
    double variance = sumSq / double(n) - mean * mean;
    if (variance < 0.0) variance = 0.0; // fp guard
    stats.mean = mean;
    stats.stddev = std::sqrt(variance);
    return stats;
}

bool isAlive(const LumaStats& s) {
    return s.mean >= 0.01 && s.mean <= 0.7 && s.stddev >= 0.02;
}

json activityToJson(const LumaStats& s) {
    return json{{"meanLuma", s.mean}, {"stddevLuma", s.stddev}, {"alive", isAlive(s)}};
}

// ---- sweep config (spec §1) ----------------------------------------------

struct AxisSpec {
    std::string target;      // "lenia0.growthMu"
    std::vector<json> values; // resolved value list (values[] verbatim, or range+steps expanded)
};

struct SweepConfig {
    std::string mode = "cartesian"; // "cartesian" | "per-axis"
    std::vector<uint32_t> seeds;    // empty => scene's own seed only
    std::vector<AxisSpec> axes;
};

bool resolveAxis(const json& axisJson, AxisSpec& out, std::string& err) {
    out.target = axisJson.value("target", std::string());
    if (out.target.empty()) {
        err = "sweep axis missing \"target\"";
        return false;
    }
    if (out.target.find('.') == std::string::npos) {
        err = "sweep axis target \"" + out.target + "\" must be \"module.param\"";
        return false;
    }
    if (axisJson.contains("values")) {
        const json& vals = axisJson.at("values");
        if (!vals.is_array() || vals.empty()) {
            err = "axis \"" + out.target + "\": \"values\" must be a non-empty array";
            return false;
        }
        for (const auto& v : vals) out.values.push_back(v);
        return true;
    }
    if (axisJson.contains("range")) {
        const json& r = axisJson.at("range");
        if (!r.is_array() || r.size() != 2 || !r[0].is_number() || !r[1].is_number()) {
            err = "axis \"" + out.target + "\": \"range\" must be [lo, hi]";
            return false;
        }
        const double lo = r[0].get<double>();
        const double hi = r[1].get<double>();
        const int steps = axisJson.value("steps", 0);
        if (steps < 1) {
            err = "axis \"" + out.target + "\": \"steps\" must be >= 1";
            return false;
        }
        if (steps == 1) {
            out.values.push_back(json(lo));
        } else {
            // Linear, both ends included.
            for (int i = 0; i < steps; ++i) {
                const double t = double(i) / double(steps - 1);
                out.values.push_back(json(lo + t * (hi - lo)));
            }
        }
        return true;
    }
    err = "axis \"" + out.target + "\": need \"values\" or \"range\"+\"steps\"";
    return false;
}

bool loadSweep(const std::string& path, SweepConfig& out, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open sweep file: " + path;
        return false;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        err = std::string("sweep JSON parse error: ") + e.what();
        return false;
    }
    try {
        out.mode = j.value("mode", std::string("cartesian"));
        if (out.mode != "cartesian" && out.mode != "per-axis") {
            err = "sweep \"mode\" must be \"cartesian\" or \"per-axis\" (got \"" + out.mode + "\")";
            return false;
        }
        if (j.contains("seeds")) {
            if (!j["seeds"].is_array()) {
                err = "sweep \"seeds\" must be an array";
                return false;
            }
            for (const auto& s : j["seeds"]) out.seeds.push_back(s.get<uint32_t>());
        }
        if (j.contains("axes")) {
            if (!j["axes"].is_array()) {
                err = "sweep \"axes\" must be an array";
                return false;
            }
            for (const auto& a : j["axes"]) {
                AxisSpec axis;
                if (!resolveAxis(a, axis, err)) return false;
                out.axes.push_back(std::move(axis));
            }
        }
    } catch (const std::exception& e) {
        err = std::string("sweep JSON structure error: ") + e.what();
        return false;
    }
    return true;
}

// ---- variant enumeration ---------------------------------------------------

struct Variant {
    std::string id;
    uint32_t seed = 0;
    std::vector<std::pair<std::string, json>> overrides; // ordered "module.param" -> value
};

// Cartesian product of value-indices across axes (odometer, last axis
// fastest). Empty axes list -> one empty combination (pure seed sweep).
std::vector<std::vector<size_t>> cartesianIndices(const std::vector<AxisSpec>& axes) {
    std::vector<std::vector<size_t>> result;
    if (axes.empty()) {
        result.push_back({});
        return result;
    }
    for (const auto& ax : axes)
        if (ax.values.empty()) return result; // no combinations possible
    std::vector<size_t> counter(axes.size(), 0);
    for (;;) {
        result.push_back(counter);
        int a = int(axes.size()) - 1;
        while (a >= 0) {
            counter[size_t(a)]++;
            if (counter[size_t(a)] < axes[size_t(a)].values.size()) break;
            counter[size_t(a)] = 0;
            a--;
        }
        if (a < 0) break; // wrapped all the way around: enumerated everything
    }
    return result;
}

std::vector<Variant> buildVariants(const SweepConfig& sweep, uint32_t baseSeed) {
    const std::vector<uint32_t> seeds =
        sweep.seeds.empty() ? std::vector<uint32_t>{baseSeed} : sweep.seeds;
    std::vector<Variant> variants;

    if (sweep.mode == "cartesian") {
        const auto combos = cartesianIndices(sweep.axes);
        for (uint32_t seed : seeds) {
            for (const auto& combo : combos) {
                Variant v;
                v.seed = seed;
                for (size_t a = 0; a < sweep.axes.size(); ++a)
                    v.overrides.emplace_back(sweep.axes[a].target, sweep.axes[a].values[combo[a]]);
                variants.push_back(std::move(v));
            }
        }
    } else { // "per-axis": vary one axis at a time from the base scene.
        for (uint32_t seed : seeds) {
            for (const auto& axis : sweep.axes) {
                for (const auto& val : axis.values) {
                    Variant v;
                    v.seed = seed;
                    v.overrides.emplace_back(axis.target, val);
                    variants.push_back(std::move(v));
                }
            }
        }
    }

    for (size_t i = 0; i < variants.size(); ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "v%04zu", i);
        variants[i].id = buf;
    }
    return variants;
}

json overridesToJson(const Variant& v) {
    json out = json::object();
    for (const auto& ov : v.overrides) out[ov.first] = ov.second;
    return out;
}

std::string overridesLogLabel(const Variant& v) {
    std::string s;
    for (const auto& ov : v.overrides) {
        if (!s.empty()) s += " ";
        s += ov.first + "=" + ov.second.dump();
    }
    if (s.empty()) s = "(base)";
    return s;
}

// ---- scene mutation (spec §2: direct scene JSON mutation, not ParameterBus)

bool hasModule(const Scene& scene, const std::string& name) {
    for (const auto& m : scene.modules)
        if (m.name == name) return true;
    return false;
}

std::string listModuleNames(const Scene& scene) {
    std::string s;
    for (const auto& m : scene.modules) {
        if (!s.empty()) s += ", ";
        s += m.name;
    }
    return s;
}

bool applyOverrides(Scene& scene, const Variant& variant, std::string& err) {
    scene.seed = variant.seed;
    for (const auto& ov : variant.overrides) {
        const std::string& target = ov.first;
        const auto dot = target.find('.');
        const std::string modName = target.substr(0, dot);
        const std::string paramKey = target.substr(dot + 1);
        ModuleSpec* found = nullptr;
        for (auto& m : scene.modules) {
            if (m.name == modName) {
                found = &m;
                break;
            }
        }
        if (!found) {
            err = "unknown module \"" + modName + "\" (target \"" + target + "\")";
            return false;
        }
        found->params[paramKey] = ov.second;
    }
    return true;
}

// ---- misc CLI/formatting helpers ------------------------------------------

std::string thumbFilename(const std::string& id, uint32_t frame) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_f%06u.png", id.c_str(), frame);
    return buf;
}

bool parseThumbAt(const std::string& s, uint32_t frames, std::vector<uint32_t>& out,
                  std::string& err) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        try {
            size_t consumed = 0;
            long v = std::stol(item, &consumed);
            if (consumed != item.size() || v < 0) throw std::invalid_argument(item);
            out.push_back(uint32_t(v));
        } catch (const std::exception&) {
            err = "invalid --thumb-at value: \"" + item + "\"";
            return false;
        }
    }
    if (out.empty()) {
        err = "--thumb-at produced no frame indices";
        return false;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    for (uint32_t f : out) {
        if (f >= frames) {
            err = "--thumb-at frame " + std::to_string(f) + " out of range for --frames " +
                  std::to_string(frames);
            return false;
        }
    }
    return true;
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// JSON embedded verbatim as a JS object literal inside a <script> tag. Guard
// against a string value that happens to contain "</" prematurely closing the
// tag; ensure_ascii keeps the rest of the file (and any exotic characters
// from user-authored sweep/override values) plain ASCII.
std::string jsonForScriptTag(const json& j) {
    const std::string s = j.dump(-1, ' ', /*ensure_ascii=*/true);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<' && i + 1 < s.size() && s[i + 1] == '/') {
            out += "<\\/";
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- index.html (spec §4: self-contained, no CDN, inline CSS/JS only) -----

std::string buildIndexHtml(const json& doc) {
    const std::string scene = doc.value("scene", std::string());
    const std::string sweep = doc.value("sweep", std::string());
    const std::string mode = doc.value("mode", std::string());

    std::string html;
    html += "<!doctype html>\n<html lang=\"en\">\n<head>\n";
    html += "<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += "<title>LifePresetLab - " + htmlEscape(scene) + "</title>\n";
    html += "<style>\n";
    html += R"CSS(
:root { color-scheme: dark; }
* { box-sizing: border-box; }
html, body { margin: 0; padding: 0; }
body {
  background: #0b0c0f;
  color: #e8e8ea;
  font: 13px/1.4 -apple-system, "SF Mono", Menlo, Consolas, monospace;
}
header {
  position: sticky; top: 0; z-index: 5;
  display: flex; align-items: center; gap: 12px; flex-wrap: wrap;
  padding: 10px 16px;
  background: #16181d;
  border-bottom: 1px solid #2a2d35;
}
header h1 { font-size: 14px; margin: 0; font-weight: 600; color: #5ad1e6; }
header .meta { color: #8b8f99; font-size: 12px; }
header .spacer { flex: 1; }
button.sortbtn {
  background: transparent; color: #e8e8ea; border: 1px solid #2a2d35;
  border-radius: 6px; padding: 5px 10px; cursor: pointer; font: inherit;
}
button.sortbtn.active { border-color: #5ad1e6; color: #5ad1e6; }
#grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
  gap: 10px;
  padding: 14px;
}
.card {
  background: #16181d; border: 1px solid #2a2d35; border-radius: 8px;
  overflow: hidden; cursor: pointer; transition: border-color .15s;
}
.card:hover { border-color: #5ad1e6; }
.card.dead { opacity: .35; }
.card img { display: block; width: 100%; height: auto; background: #000; }
.card .meta { padding: 6px 8px; }
.card .id { color: #5ad1e6; font-weight: 600; margin-right: 6px; }
.card .lbl { color: #e8e8ea; word-break: break-all; }
.card .stat { display: block; color: #8b8f99; margin-top: 3px; font-size: 11px; }
#empty { padding: 40px; color: #8b8f99; text-align: center; }
#detail {
  position: fixed; inset: 0; background: rgba(0,0,0,.75);
  display: none; align-items: center; justify-content: center; padding: 24px; z-index: 10;
}
#detail.open { display: flex; }
#detail .box {
  background: #16181d; border: 1px solid #2a2d35; border-radius: 8px;
  max-width: 640px; max-height: 80vh; overflow: auto; padding: 16px;
}
#detail pre { margin: 0; white-space: pre-wrap; word-break: break-all; }
#detail .close { float: right; cursor: pointer; color: #8b8f99; }
)CSS";
    html += "</style>\n</head>\n<body>\n";

    html += "<header>\n";
    html += "  <h1>LifePresetLab</h1>\n";
    html += "  <span class=\"meta\">scene " + htmlEscape(scene) + " &middot; sweep " +
            htmlEscape(sweep) + " &middot; mode " + htmlEscape(mode) + "</span>\n";
    html += "  <span class=\"spacer\"></span>\n";
    html += "  <span id=\"count\" class=\"meta\"></span>\n";
    html += "  <button class=\"sortbtn active\" data-sort=\"activity\">activity</button>\n";
    html += "  <button class=\"sortbtn\" data-sort=\"id\">id</button>\n";
    html += "  <button class=\"sortbtn\" data-sort=\"gpuMs\">gpu ms</button>\n";
    html += "</header>\n";
    html += "<div id=\"grid\"></div>\n";
    html += "<div id=\"detail\">\n  <div class=\"box\">\n";
    html += "    <span id=\"detail-close\" class=\"close\">close &times;</span>\n";
    html += "    <h2 id=\"detail-title\" style=\"margin:0 0 10px 0;color:#5ad1e6;\"></h2>\n";
    html += "    <pre id=\"detail-pre\"></pre>\n";
    html += "  </div>\n</div>\n";

    html += "<script>\nconst DATA = ";
    html += jsonForScriptTag(doc);
    html += ";\n";
    html += R"JS(
(function () {
  var variants = DATA.variants || [];
  var grid = document.getElementById('grid');
  var countEl = document.getElementById('count');
  var sortState = 'activity';

  function fmt(n) { return (typeof n === 'number') ? n.toFixed(3) : String(n); }

  function shortLabel(v) {
    var keys = Object.keys(v.overrides || {});
    if (keys.length === 0) return '(base) seed=' + v.seed;
    return keys.map(function (k) {
      var dot = k.indexOf('.');
      var short = dot >= 0 ? k.slice(dot + 1) : k;
      return short + '=' + v.overrides[k];
    }).join(' ');
  }

  function render() {
    var arr = variants.slice();
    if (sortState === 'activity') {
      arr.sort(function (a, b) { return (b.activity.stddevLuma || 0) - (a.activity.stddevLuma || 0); });
    } else if (sortState === 'id') {
      arr.sort(function (a, b) { return a.id < b.id ? -1 : (a.id > b.id ? 1 : 0); });
    } else if (sortState === 'gpuMs') {
      arr.sort(function (a, b) { return (b.avgGpuMs || 0) - (a.avgGpuMs || 0); });
    }

    grid.textContent = '';
    if (arr.length === 0) {
      var empty = document.createElement('div');
      empty.id = 'empty';
      empty.textContent = 'No variants.';
      grid.appendChild(empty);
    }
    arr.forEach(function (v) {
      var act = v.activity || {};
      var card = document.createElement('div');
      card.className = 'card' + (act.alive ? '' : ' dead');

      var img = document.createElement('img');
      var thumb = (v.thumbs && v.thumbs.length) ? v.thumbs[v.thumbs.length - 1] : '';
      img.src = thumb;
      img.loading = 'lazy';
      img.alt = v.id;
      card.appendChild(img);

      var meta = document.createElement('div');
      meta.className = 'meta';

      var idSpan = document.createElement('span');
      idSpan.className = 'id';
      idSpan.textContent = v.id;
      meta.appendChild(idSpan);

      var lblSpan = document.createElement('span');
      lblSpan.className = 'lbl';
      lblSpan.textContent = shortLabel(v);
      meta.appendChild(lblSpan);

      var statSpan = document.createElement('span');
      statSpan.className = 'stat';
      statSpan.textContent = 'seed=' + v.seed + '  mean=' + fmt(act.meanLuma) +
        ' stddev=' + fmt(act.stddevLuma) + '  gpu=' + fmt(v.avgGpuMs) + 'ms';
      meta.appendChild(statSpan);

      card.appendChild(meta);
      card.addEventListener('click', function () { showDetail(v); });
      grid.appendChild(card);
    });

    var aliveCount = arr.filter(function (v) { return v.activity && v.activity.alive; }).length;
    countEl.textContent = arr.length + ' variants (' + aliveCount + ' alive)';
  }

  function showDetail(v) {
    document.getElementById('detail-title').textContent = v.id + '  (seed ' + v.seed + ')';
    document.getElementById('detail-pre').textContent = JSON.stringify(v.overrides, null, 2);
    document.getElementById('detail').classList.add('open');
  }

  document.getElementById('detail-close').addEventListener('click', function () {
    document.getElementById('detail').classList.remove('open');
  });
  document.getElementById('detail').addEventListener('click', function (e) {
    if (e.target.id === 'detail') e.currentTarget.classList.remove('open');
  });

  var btns = document.querySelectorAll('.sortbtn');
  for (var i = 0; i < btns.length; i++) {
    btns[i].addEventListener('click', function (e) {
      sortState = e.currentTarget.getAttribute('data-sort');
      for (var j = 0; j < btns.length; j++) btns[j].classList.remove('active');
      e.currentTarget.classList.add('active');
      render();
    });
  }

  render();
})();
)JS";
    html += "</script>\n</body>\n</html>\n";
    return html;
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"LifePresetLab - parameter/seed sweep runner + activity gallery"};

    std::string scenePath = "Presets/default.json";
    std::string sweepPath;
    std::string shaderRoot;
    std::string audioCapture;
    std::string outputDir = "lab/out";
    std::string thumbAtArg;
    int width = -1, height = -1;
    double fps = 30.0;
    uint32_t frames = 240;
    uint32_t substeps = 1;
    float exposure = 1.0f;
    bool aliveOnly = false;
    bool resume = false;

    app.add_option("--scene", scenePath, "Scene JSON path");
    app.add_option("--sweep", sweepPath, "Sweep JSON path")->required();
    app.add_option("--shaders", shaderRoot, "Shaders/ directory");
    app.add_option("--frames", frames, "Frames to step per variant");
    app.add_option("--thumb-at", thumbAtArg,
                   "Comma list of frame indices to capture (default: last frame only)");
    app.add_option("--width", width, "Override scene width");
    app.add_option("--height", height, "Override scene height");
    app.add_option("--fps", fps, "Simulation frame rate (fixed dt = 1/fps)");
    app.add_option("--substeps", substeps, "Simulation substeps per frame");
    app.add_option("--audio", audioCapture, "AudioFeatureState capture (.jsonl) to replay");
    app.add_option("--output", outputDir, "Output directory");
    app.add_option("--exposure", exposure, "PNG exposure");
    app.add_flag("--alive-only", aliveOnly,
                "Delete thumbnail PNGs for non-alive variants once the run is done");
    app.add_flag("--resume", resume, "Skip writing thumbnail PNGs that already exist on disk");
    CLI11_PARSE(app, argc, argv);

    if (frames == 0) {
        fprintf(stderr, "[lab] --frames must be >= 1\n");
        return 1;
    }

    modules::registerBuiltinModules();

    std::string err;
    auto scene = Scene::loadFile(scenePath, err);
    if (!scene) {
        fprintf(stderr, "[lab] %s\n", err.c_str());
        return 1;
    }
    if (width > 0) scene->width = uint32_t(width);
    if (height > 0) scene->height = uint32_t(height);

    // ---- sweep config -------------------------------------------------
    SweepConfig sweep;
    if (!loadSweep(sweepPath, sweep, err)) {
        fprintf(stderr, "[lab] %s\n", err.c_str());
        return 1;
    }

    // Validate every axis target's module exists BEFORE running anything
    // (spec §2 / §6-5): fail fast with a clear message, exit 1.
    for (const auto& axis : sweep.axes) {
        const auto dot = axis.target.find('.');
        const std::string modName = axis.target.substr(0, dot);
        if (!hasModule(*scene, modName)) {
            fprintf(stderr,
                    "[lab] sweep axis target \"%s\": module \"%s\" not found in scene "
                    "(available: %s)\n",
                    axis.target.c_str(), modName.c_str(), listModuleNames(*scene).c_str());
            return 1;
        }
    }

    // ---- thumb-at frames ------------------------------------------------
    std::vector<uint32_t> thumbFrames;
    if (thumbAtArg.empty()) {
        thumbFrames.push_back(frames - 1);
    } else if (!parseThumbAt(thumbAtArg, frames, thumbFrames, err)) {
        fprintf(stderr, "[lab] %s\n", err.c_str());
        return 1;
    }

    // ---- variant enumeration + the >500 safety cap (spec §1) ------------
    std::vector<Variant> variants = buildVariants(sweep, scene->seed);
    if (variants.empty()) {
        fprintf(stderr, "[lab] sweep produced 0 variants (check \"axes\"/\"seeds\")\n");
        return 1;
    }
    if (variants.size() > 500) {
        fprintf(stderr, "[lab] refusing to run %zu variants (limit 500)\n", variants.size());
        return 1;
    }

    if (!app::ensureDirectory(outputDir, err)) {
        fprintf(stderr, "[lab] %s\n", err.c_str());
        return 1;
    }

    CaptureReader capture;
    bool hasCapture = false;
    if (!audioCapture.empty()) {
        if (!capture.load(audioCapture, err)) {
            fprintf(stderr, "[lab] %s\n", err.c_str());
            return 1;
        }
        hasCapture = true;
        fprintf(stderr, "[lab] audio capture: %zu frames\n", capture.frameCount());
    }

    const std::string shaderRootResolved = app::resolveShaderRoot(shaderRoot, argv[0]);
    const float dt = float(1.0 / fps);

    fprintf(stderr,
            "[lab] scene=%s sweep=%s mode=%s -> %zu variants | %ux%u %u frames @ %.1ffps\n",
            scenePath.c_str(), sweepPath.c_str(), sweep.mode.c_str(), variants.size(),
            scene->width, scene->height, frames, fps);

    auto wallStart = std::chrono::steady_clock::now();
    std::string deviceName;
    json variantEntries = json::array();
    size_t aliveCount = 0;

    for (size_t vi = 0; vi < variants.size(); ++vi) {
        const Variant& variant = variants[vi];

        json entry;
        entry["id"] = variant.id;
        entry["seed"] = variant.seed;
        entry["overrides"] = overridesToJson(variant);
        entry["thumbs"] = json::array();
        entry["avgGpuMs"] = 0.0;

        Scene variantScene = *scene;
        std::string variantErr;
        std::unique_ptr<SceneRunner> runner;
        if (!applyOverrides(variantScene, variant, variantErr)) {
            fprintf(stderr, "[lab] %s: %s\n", variant.id.c_str(), variantErr.c_str());
        } else {
            SceneRunnerDesc rd;
            rd.scene = variantScene;
            rd.shaderRoot = shaderRootResolved;
            rd.substeps = substeps;
            runner = SceneRunner::create(rd, variantErr);
            if (!runner) fprintf(stderr, "[lab] %s: %s\n", variant.id.c_str(), variantErr.c_str());
        }

        LumaStats lastStats;

        if (!runner) {
            entry["error"] = variantErr;
            entry["activity"] = activityToJson(lastStats);
        } else {
            if (deviceName.empty()) deviceName = runner->metal().deviceName();

            FrameRecorder recorder(runner->resources());
            double gpuMsSum = 0.0;
            json thumbs = json::array();

            for (uint32_t f = 0; f < frames; ++f) {
                AudioFeatureState state;
                if (hasCapture) state = capture.frame(f);
                state.time = float(f) * dt;
                state.deltaTime = dt;
                state.frameIndex = f;

                runner->step(state, dt); // readback handled separately below (own FrameRecorder)
                gpuMsSum += runner->graph().lastFrameGPUms();

                if (!std::binary_search(thumbFrames.begin(), thumbFrames.end(), f)) continue;

                // Own beginFrame/encodeReadback/endFrame bracket, exactly the
                // contract FrameRecorder::encodeReadback documents — the
                // simulation frame is already fully computed and sitting in
                // outputTexture(), so this blit-only frame cannot perturb
                // simulation state or determinism.
                runner->graph().beginFrame(f);
                recorder.encodeReadback(runner->graph(), runner->outputTexture());
                runner->graph().endFrame(true);
                recorder.fetch();

                const std::string fname = thumbFilename(variant.id, f);
                thumbs.push_back(fname);
                const std::string pngPath = (fs::path(outputDir) / fname).string();

                const bool skipWrite = resume && fs::exists(pngPath);
                if (!skipWrite) {
                    std::string werr;
                    if (!recorder.writePNG(pngPath, exposure, werr))
                        fprintf(stderr, "[lab] %s frame %u: %s\n", variant.id.c_str(), f,
                                werr.c_str());
                }

                // Activity is always freshly computed from raw halfPixels()
                // (never from a re-decoded PNG, resumed or not — spec §3/§5).
                lastStats = computeLumaStats(recorder.halfPixels(), recorder.width(),
                                             recorder.height());
            }

            entry["thumbs"] = thumbs;
            entry["avgGpuMs"] = frames ? gpuMsSum / double(frames) : 0.0;
            entry["activity"] = activityToJson(lastStats);
        }

        const bool alive = entry["activity"].value("alive", false);
        if (alive) aliveCount++;
        variantEntries.push_back(entry);

        fprintf(stderr, "[lab] v%04zu/%04zu seed=%u %s gpu %.2fms alive=%d\n", vi,
                variants.size() - 1, variant.seed, overridesLogLabel(variant).c_str(),
                entry["avgGpuMs"].get<double>(), alive ? 1 : 0);
    }

    // --alive-only: delete non-alive thumbnails once the whole run is done;
    // variants.json keeps every entry (including activity) regardless.
    if (aliveOnly) {
        size_t deleted = 0;
        for (const auto& entry : variantEntries) {
            if (entry.value("activity", json::object()).value("alive", false)) continue;
            for (const auto& thumb : entry.value("thumbs", json::array())) {
                std::error_code ec;
                fs::remove(fs::path(outputDir) / thumb.get<std::string>(), ec);
                if (!ec) deleted++;
            }
        }
        fprintf(stderr, "[lab] --alive-only: removed %zu thumbnail(s)\n", deleted);
    }

    json doc;
    doc["scene"] = scenePath;
    doc["sweep"] = sweepPath;
    doc["mode"] = sweep.mode;
    doc["frames"] = frames;
    doc["thumbAt"] = thumbFrames;
    doc["width"] = scene->width;
    doc["height"] = scene->height;
    doc["fps"] = fps;
    doc["substeps"] = substeps;
    doc["audioCapture"] = audioCapture;
    doc["device"] = deviceName;
    doc["aliveOnly"] = aliveOnly;
    doc["resume"] = resume;
    doc["variantCount"] = variantEntries.size();
    doc["variants"] = variantEntries;

    std::ofstream variantsFile(fs::path(outputDir) / "variants.json");
    variantsFile << doc.dump(2) << "\n";

    std::ofstream htmlFile(fs::path(outputDir) / "index.html");
    htmlFile << buildIndexHtml(doc);

    auto wallEnd = std::chrono::steady_clock::now();
    fprintf(stderr, "[lab] done: %zu variants, %zu alive, wall %.1fs -> %s\n",
            variantEntries.size(), aliveCount,
            std::chrono::duration<double>(wallEnd - wallStart).count(), outputDir.c_str());
    return 0;
}
