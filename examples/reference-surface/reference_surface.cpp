// Reference-Surface validator (Phase 7, headless core).
//
// Google's MRT2 reference apps demonstrate distinct slices of the control surface:
//   • Jam      — MIDI note conditioning
//   • Collider — prompt-space exploration (multi-prompt blend, PCA)
//   • all-in-one — text prompts, drumless/onset modes
// The faithful cross-format React-UI clones are a follow-on; THIS proves the underlying
// `magentart::core` control surface works end-to-end (the apps' actual substance), so a
// UI clone is wiring, not capability risk. Each surface is exercised then rendered; we
// assert the model still produces non-silent audio under that conditioning.
//
//   usage: reference-surface <model.mlxfn> <resources_dir>

#include <magentart/mlx_engine.h>
#include <magentart/detail/autorelease_pool.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using magentart::core::MLXEngine;
using magentart::core::kFrameSamples;

namespace {
void wait_encode(MLXEngine& e) {
    while (e.get_text_encoder_status() == 1 || e.get_quantizer_status() == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
// Render `frames` and return peak amplitude (proxy for "non-silent").
double render_peak(MLXEngine& e, int frames) {
    std::vector<float> L(kFrameSamples), R(kFrameSamples);
    double peak = 0;
    for (int f = 0; f < frames; ++f) {
        magentart::detail::AutoreleasePool pool;
        if (!e.generate_frame(L.data(), R.data())) return -1;
        for (float x : L) peak = std::max(peak, (double)std::fabs(x));
    }
    return peak;
}
bool check(const char* surface, double peak, int& passed) {
    bool ok = peak > 1e-3;
    std::printf("  [%s] %s (peak=%.3f)\n", ok ? "PASS" : "FAIL", surface, peak);
    if (ok) ++passed;
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <model.mlxfn> <resources>\n", argv[0]); return 2; }
    MLXEngine engine;
    if (!engine.init_assets(argv[2], "musiccoca")) { std::fprintf(stderr, "FAIL: init_assets\n"); return 1; }
    if (!engine.load_model(argv[1]))               { std::fprintf(stderr, "FAIL: load_model\n");  return 1; }

    int passed = 0, total = 0;
    std::printf("Reference-surface validation (Jam / Collider / all-in-one control surface):\n");

    // 1) Text prompt (all-in-one baseline).
    ++total;
    engine.set_text_prompt("warm analog pads"); wait_encode(engine);
    check("text-prompt", render_peak(engine, 6), passed);

    // 2) Multi-prompt BLEND — Collider's prompt-space (two styles weighted).
    ++total;
    engine.set_text_prompts({"warm analog pads", "driving techno groove"}, {0.6f, 0.4f}); wait_encode(engine);
    check("prompt-blend (Collider)", render_peak(engine, 6), passed);

    // 3) MIDI note conditioning — Jam.
    ++total;
    engine.set_note_on(60); engine.set_note_on(64); engine.set_note_on(67);   // C-major triad
    double p = render_peak(engine, 6);
    engine.set_note_off(60); engine.set_note_off(64); engine.set_note_off(67);
    check("note-conditioning (Jam)", p, passed);

    // 4) Drumless + onset mode toggles — all-in-one controls.
    ++total;
    engine.set_drumless(true); engine.set_onset_mode(1);
    bool drumless_ok = check("drumless + onset-mode", render_peak(engine, 6), passed);
    engine.set_drumless(false); engine.set_onset_mode(0);
    (void)drumless_ok;

    std::printf("Reference surface: %d/%d control slices validated.\n", passed, total);
    if (passed < total) { std::printf("FAIL: a control surface produced silence\n"); return 1; }
    std::printf("OK: full reference control surface (prompt/blend/notes/modes) generates audio\n");
    return 0;
}
