// Continuation Effect (E3) — headless prefill → generated continuation.
//
// The core of the "Keep Jamming" effect: capture a phrase, prefill the model's state from it,
// and generate a continuation in that style. Here (self-contained, no host capture) we generate
// a short SEED clip, prefill the engine from it, then generate the CONTINUATION — proving the
// audio-prefill → continuation mechanism via `magentart::core::MLXEngine`.
//
// In a real plugin this is async (prefill_state blocks; it stops/encodes/restarts the engine),
// so the host runs it on a worker thread, never in the audio callback — see the delivery plan's
// E3 RT caveats. This headless tool validates the generation mechanism, not the RT plumbing.
//
//   usage: continuation <model.mlxfn> <resources_dir> <spectrostream_encoder.mlxfn> [out.wav]

#include <magentart/mlx_engine.h>
#include <magentart/detail/autorelease_pool.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using magentart::core::MLXEngine;
using magentart::core::kFrameSamples;

void write_wav_f32(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
    const uint32_t sr = 48000, ch = 2, bits = 32, n = (uint32_t)L.size();
    const uint32_t data_bytes = n * ch * (bits / 8);
    std::ofstream o(path, std::ios::binary);
    auto u32 = [&](uint32_t v){ o.write((char*)&v, 4); };
    auto u16 = [&](uint16_t v){ o.write((char*)&v, 2); };
    o.write("RIFF", 4); u32(36 + data_bytes); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(16); u16(3); u16((uint16_t)ch); u32(sr);
    u32(sr * ch * (bits/8)); u16((uint16_t)(ch * (bits/8))); u16((uint16_t)bits);
    o.write("data", 4); u32(data_bytes);
    for (uint32_t i = 0; i < n; ++i) { o.write((char*)&L[i], 4); o.write((char*)&R[i], 4); }
}
double rms(const std::vector<float>& v) {
    double s = 0; for (float x : v) s += (double)x * x; return v.empty() ? 0 : std::sqrt(s / v.size());
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <model.mlxfn> <resources> <spectrostream_encoder.mlxfn> [out.wav]\n", argv[0]);
        return 2;
    }
    const std::string model = argv[1], resources = argv[2], sstream = argv[3];
    const std::string out = (argc > 4) ? argv[4] : "continuation.wav";
    const int seed_frames = 25, cont_frames = 25;   // 1 s seed + 1 s continuation

    (void)sstream;   // token-prefill path skips the SpectroStream encoder (no fixed 28s shape)
    MLXEngine engine;
    if (!engine.init_assets(resources.c_str(), "musiccoca"))         { std::fprintf(stderr, "FAIL: init_assets\n"); return 1; }
    if (!engine.load_model(model.c_str()))                           { std::fprintf(stderr, "FAIL: load_model\n");  return 1; }
    engine.set_text_prompt("driving techno groove");
    while (engine.get_text_encoder_status() == 1 || engine.get_quantizer_status() == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int rvq = engine.get_rvq_depth();   // tokens per frame

    // 1) Generate a SEED phrase, capturing the raw RVQ tokens (lossless branch material).
    std::vector<float> L, R, fL(kFrameSamples), fR(kFrameSamples);
    std::vector<int32_t> seedTokens; seedTokens.reserve((size_t)seed_frames * rvq);
    std::vector<int32_t> tok(magentart::core::kNumRVQLevels);
    for (int f = 0; f < seed_frames; ++f) {
        magentart::detail::AutoreleasePool pool;
        if (!engine.generate_frame(fL.data(), fR.data(), tok.data())) { std::fprintf(stderr, "FAIL: seed gen %d\n", f); return 1; }
        for (int k = 0; k < rvq; ++k) seedTokens.push_back(tok[k]);
        L.insert(L.end(), fL.begin(), fL.end()); R.insert(R.end(), fR.begin(), fR.end());
    }
    std::vector<float> seedL = L;   // for assertion

    // 2) Prefill state from the captured seed TOKENS — lossless, no encoder, no fixed shape.
    std::printf("Prefilling from %d-frame seed (%d tokens)...\n", seed_frames, (int)seedTokens.size());
    bool ok = engine.prefill_state_from_tokens(seedTokens.data(), seed_frames);
    if (!ok) { std::fprintf(stderr, "FAIL: prefill_state_from_tokens\n"); return 1; }

    // 3) Generate the CONTINUATION from the prefilled state.
    std::vector<float> cL, cR;
    for (int f = 0; f < cont_frames; ++f) {
        magentart::detail::AutoreleasePool pool;
        if (!engine.generate_frame(fL.data(), fR.data())) { std::fprintf(stderr, "FAIL: cont gen %d\n", f); return 1; }
        cL.insert(cL.end(), fL.begin(), fL.end()); cR.insert(cR.end(), fR.begin(), fR.end());
        L.insert(L.end(), fL.begin(), fL.end());   R.insert(R.end(), fR.begin(), fR.end());
    }

    write_wav_f32(out, L, R);   // seed + continuation

    double rseed = rms(seedL), rcont = rms(cL), peak = 0;
    for (float x : cL) peak = std::max(peak, (double)std::fabs(x));
    bool ok_audio = (rcont > 1e-4 && peak > 1e-3);
    std::printf("seed %d + continuation %d frames -> %s\n", seed_frames, cont_frames, out.c_str());
    std::printf("  seed_rms=%.4f  continuation_rms=%.4f  cont_peak=%.3f  non_silent=%d\n",
                rseed, rcont, peak, ok_audio);
    if (!ok_audio) { std::printf("FAIL: continuation is silent (prefill/generate path broken)\n"); return 1; }
    std::printf("OK: prefilled from seed audio and generated a non-silent continuation\n");
    return 0;
}
