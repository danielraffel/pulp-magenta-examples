// Agent-Conducted Session (E2) — headless conductor / replay.
//
// Testable core of the flagship "agent conducts a live MRT2 instance" demo. An agent's steer
// is a typed numeric command (temperature / top-k / cfg) — exactly what G1's
// `pulp_inspect_set_param` sends to a running plugin over MCP. We capture that command stream
// as a `.pulpset` (timestamped commands) and *replay* it headlessly, rendering audio and
// asserting a range/property regression. Demo == test: the live MCP session and this replay
// drive the SAME commands; only the transport differs (MCP→plugin vs direct engine).
//
// Drives `magentart::core::MLXEngine` directly (single-threaded, like hello_mrt2) for
// deterministic offline rendering — RealtimeRunner's inference thread is for live hosts.
//
// Scope (E2 v1, per the delivery plan): NUMERIC steering of an already-loaded model. Live
// text-prompt / model-switch conducting is the G5 follow-on.
//
//   usage: conductor <model.mlxfn> <resources_dir> <session.pulpset> [out.wav]
//
// `.pulpset` lines: `<frame> <command> <value...>`  ('#' comments); commands:
//   prompt <text...>   (applied once, before render)
//   temperature <float> | top_k <int>
//   cfg_musiccoca <float> | cfg_notes <float> | cfg_drums <float>
//   note_on <pitch> | note_off <pitch>

#include <magentart/mlx_engine.h>
#include <magentart/detail/autorelease_pool.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using magentart::core::MLXEngine;
using magentart::core::kFrameSamples;

struct Command {
    int frame = 0;
    std::string op;
    std::string text;
    double a = 0.0;
};

std::vector<Command> parse_pulpset(const std::string& path, std::string& initial_prompt) {
    std::vector<Command> cmds;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::istringstream ls(line);
        Command c;
        if (!(ls >> c.frame >> c.op)) continue;
        if (c.op == "prompt") {
            std::getline(ls, c.text);
            size_t s = c.text.find_first_not_of(" \t\"");
            size_t e = c.text.find_last_not_of(" \t\"");
            c.text = (s == std::string::npos) ? "" : c.text.substr(s, e - s + 1);
            if (c.frame == 0 && initial_prompt.empty()) initial_prompt = c.text;
        } else {
            ls >> c.a;
        }
        cmds.push_back(c);
    }
    return cmds;
}

void apply(MLXEngine& e, const Command& c) {
    if (c.op == "temperature")        e.set_temperature((float)c.a);
    else if (c.op == "top_k")         e.set_top_k((int)c.a);
    else if (c.op == "cfg_musiccoca") e.set_cfg_musiccoca((float)c.a);
    else if (c.op == "cfg_notes")     e.set_cfg_notes((float)c.a);
    else if (c.op == "cfg_drums")     e.set_cfg_drums((float)c.a);
    else if (c.op == "note_on")       e.set_note_on((int)c.a);
    else if (c.op == "note_off")      e.set_note_off((int)c.a);
}

void write_wav_f32(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
    const uint32_t sr = 48000, ch = 2, bits = 32;
    const uint32_t n = (uint32_t)L.size();
    const uint32_t data_bytes = n * ch * (bits / 8);
    std::ofstream o(path, std::ios::binary);
    auto u32 = [&](uint32_t v){ o.write((char*)&v, 4); };
    auto u16 = [&](uint16_t v){ o.write((char*)&v, 2); };
    o.write("RIFF", 4); u32(36 + data_bytes); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(16); u16(3); u16((uint16_t)ch); u32(sr);
    u32(sr * ch * (bits / 8)); u16((uint16_t)(ch * (bits / 8))); u16((uint16_t)bits);
    o.write("data", 4); u32(data_bytes);
    for (uint32_t i = 0; i < n; ++i) { o.write((char*)&L[i], 4); o.write((char*)&R[i], 4); }
}

double rms(const std::vector<float>& v, size_t a, size_t b) {
    double s = 0; size_t n = 0;
    for (size_t i = a; i < b && i < v.size(); ++i) { s += (double)v[i] * v[i]; ++n; }
    return n ? std::sqrt(s / n) : 0.0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <model.mlxfn> <resources> <session.pulpset> [out.wav]\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1], resources = argv[2], pulpset = argv[3];
    const std::string out = (argc > 4) ? argv[4] : "session.wav";

    std::string initial_prompt;
    auto cmds = parse_pulpset(pulpset, initial_prompt);
    if (cmds.empty()) { std::fprintf(stderr, "FAIL: empty/unreadable pulpset %s\n", pulpset.c_str()); return 1; }

    MLXEngine engine;
    if (!engine.init_assets(resources.c_str(), "musiccoca")) { std::fprintf(stderr, "FAIL: init_assets\n"); return 1; }
    if (!engine.load_model(model.c_str()))                   { std::fprintf(stderr, "FAIL: load_model\n");  return 1; }
    engine.set_text_prompt(initial_prompt.empty() ? "warm analog pads" : initial_prompt);
    while (engine.get_text_encoder_status() == 1 || engine.get_quantizer_status() == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::map<int, std::vector<Command>> by_frame;
    int last_frame = 0;
    for (auto& c : cmds) { by_frame[c.frame].push_back(c); last_frame = std::max(last_frame, c.frame); }
    const int total_frames = last_frame + 20;   // tail past the last command

    std::vector<float> L, R, fL(kFrameSamples), fR(kFrameSamples);
    L.reserve((size_t)total_frames * kFrameSamples);
    R.reserve((size_t)total_frames * kFrameSamples);

    for (int f = 0; f < total_frames; ++f) {
        magentart::detail::AutoreleasePool pool;   // drain MLX/Metal temporaries each frame
        auto it = by_frame.find(f);
        if (it != by_frame.end())
            for (auto& c : it->second) if (c.op != "prompt") apply(engine, c);
        if (!engine.generate_frame(fL.data(), fR.data())) {
            std::fprintf(stderr, "FAIL: generate_frame at %d\n", f); return 1;
        }
        L.insert(L.end(), fL.begin(), fL.end());
        R.insert(R.end(), fR.begin(), fR.end());
    }

    write_wav_f32(out, L, R);

    // Range/property regression — generation is non-deterministic, so assert shape +
    // non-silence + that the conducted commands ran without error.
    const size_t n = L.size();
    double full = rms(L, 0, n), peak = 0;
    for (float x : L) peak = std::max(peak, (double)std::fabs(x));
    bool ok_len = (n == (size_t)total_frames * kFrameSamples);
    bool ok_audio = (full > 1e-4 && peak > 1e-3);
    std::printf("conducted %zu commands over %d frames (%.2fs) -> %s\n",
                cmds.size(), total_frames, total_frames * (double)kFrameSamples / 48000.0, out.c_str());
    std::printf("  length_ok=%d  non_silent=%d  peak=%.3f  rms=%.4f  seg0=%.4f seg2=%.4f\n",
                ok_len, ok_audio, peak, full, rms(L, 0, n / 3), rms(L, 2 * n / 3, n));
    if (!ok_len)   { std::printf("FAIL: wrong sample count\n"); return 1; }
    if (!ok_audio) { std::printf("FAIL: silent output (model did not generate)\n"); return 1; }
    std::printf("OK: agent-conducted session replayed to non-silent audio\n");
    return 0;
}
