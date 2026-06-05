// Promptable Accompanist (E1) — WebView UI (Phase 7).
//
// A native-WebView editor with an inline-HTML UI: a text-prompt box + the numeric
// controls, bridged to the plugin via window.pulp.postMessage. Mirrors the proven
// embedding lifecycle from examples/webview-plugin (attach/sync/detach against the
// PluginViewHost). The Processor wires the message handler to callbacks that set
// parameters (StateStore) and the MRT2 text prompt.
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/web_view.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/runtime/log.hpp>

#include <functional>
#include <memory>
#include <string>

namespace pulp::examples::accompanist {

using SetParam  = std::function<void(std::uint32_t, float)>;
using SetPrompt = std::function<void(const std::string&)>;

inline const char* kAccompanistHtml() {
    return R"HTML(<!doctype html><html><head><meta charset="utf-8"><style>
  :root{color-scheme:dark} body{margin:0;font:13px/1.4 -apple-system,system-ui;background:#15171c;color:#e8eaf0;padding:18px}
  h1{font-size:15px;margin:0 0 2px} .sub{color:#8b90a0;font-size:11px;margin-bottom:14px}
  .row{display:flex;align-items:center;gap:10px;margin:9px 0} label{width:96px;color:#aeb4c4}
  input[type=range]{flex:1;accent-color:#6ea8fe} .val{width:48px;text-align:right;color:#9fb6ff;font-variant-numeric:tabular-nums}
  textarea{width:100%;box-sizing:border-box;background:#1e2128;color:#e8eaf0;border:1px solid #2c3140;border-radius:7px;padding:8px;resize:vertical}
  .note{margin-top:12px;color:#737a8c;font-size:11px}
</style></head><body>
  <h1>Promptable Accompanist</h1>
  <div class="sub">Magenta RealTime 2 · generative · ~200 ms latency — MIDI steers, prompt shapes the style</div>
  <div class="row"><label>Prompt</label></div>
  <textarea id="prompt" rows="2">warm analog pads</textarea>
  <div class="row"><label>Temperature</label><input id="p0" type="range" min="0.1" max="2" step="0.01" value="1.1"><span class="val" id="v0">1.10</span></div>
  <div class="row"><label>Top K</label><input id="p1" type="range" min="1" max="64" step="1" value="40"><span class="val" id="v1">40</span></div>
  <div class="row"><label>Prompt CFG</label><input id="p2" type="range" min="0" max="8" step="0.1" value="1"><span class="val" id="v2">1.0</span></div>
  <div class="row"><label>Notes CFG</label><input id="p3" type="range" min="0" max="8" step="0.1" value="1"><span class="val" id="v3">1.0</span></div>
  <div class="row"><label>Drums CFG</label><input id="p4" type="range" min="0" max="8" step="0.1" value="1"><span class="val" id="v4">1.0</span></div>
  <div class="row"><label>Volume</label><input id="p5" type="range" min="-60" max="6" step="0.1" value="0"><span class="val" id="v5">0.0</span></div>
  <div class="note" style="color:#737a8c">Audio is generated on a background thread; changes take effect within ~200 ms.</div>
<script>
  function send(t,p){ if(window.pulp&&window.pulp.postMessage) window.pulp.postMessage(t,p,"m"); }
  for(let i=0;i<6;i++){ const s=document.getElementById("p"+i), v=document.getElementById("v"+i);
    s.addEventListener("input",()=>{ v.textContent=(+s.value).toFixed(i===1?0:(i===0?2:1)); send("setParam",{id:i,value:+s.value}); }); }
  document.getElementById("prompt").addEventListener("change",e=>send("setPrompt",e.target.value));
  send("ready",{});
</script></body></html>)HTML";
}

// Extract an integer/float field from a tiny JSON object payload like {"id":2,"value":1.4}.
inline double json_num(const std::string& s, const std::string& key, double fallback) {
    auto k = "\"" + key + "\"";
    auto p = s.find(k);
    if (p == std::string::npos) return fallback;
    p = s.find(':', p);
    if (p == std::string::npos) return fallback;
    try { return std::stod(s.substr(p + 1)); } catch (...) { return fallback; }
}
inline std::string json_unquote(const std::string& s) {
    auto a = s.find('"'); if (a == std::string::npos) return s;
    auto b = s.rfind('"'); if (b <= a) return s;
    return s.substr(a + 1, b - a - 1);
}

class AccompanistPane final : public view::View {
public:
    AccompanistPane(SetParam set_param, SetPrompt set_prompt)
        : set_param_(std::move(set_param)), set_prompt_(std::move(set_prompt)) {
        view::WebViewOptions options;
        options.transparent_background = true;
        panel_ = view::WebViewPanel::create(options);
        if (!panel_) {
            runtime::log_warn("PromptableAccompanist: native WebView backend unavailable");
            return;
        }
        panel_->set_message_handler([this](const view::WebViewMessage& m) -> std::string {
            if (m.type == "setParam") {
                auto id = (std::uint32_t)json_num(m.payload_json, "id", -1);
                auto v  = (float)json_num(m.payload_json, "value", 0.0);
                if (set_param_) set_param_(id, v);
            } else if (m.type == "setPrompt") {
                if (set_prompt_) set_prompt_(json_unquote(m.payload_json));
            }
            return R"({"ok":true})";
        });
        panel_->set_ready_handler([this] { if (panel_) panel_->set_html(kAccompanistHtml()); });
    }
    ~AccompanistPane() override { detach_if_needed(); }

    void attach_if_needed() {
        auto* host = plugin_view_host();
        if (attached_ || !host || !panel_ || !panel_->native_handle()) return;
        const auto size = host->get_size();
        attached_ = host->attach_native_child_view(panel_->native_handle(), 0.0f, 0.0f,
                                                   (float)size.width, (float)size.height);
        if (attached_) sync_to_host();
    }
    void sync_to_host() {
        auto* host = plugin_view_host();
        if (!attached_ || !host || !panel_ || !panel_->native_handle()) return;
        const auto size = host->get_size();
        host->set_native_child_view_bounds(panel_->native_handle(), 0.0f, 0.0f,
                                           (float)size.width, (float)size.height);
    }
    void detach_if_needed() {
        auto* host = plugin_view_host();
        if (!attached_ || !host || !panel_ || !panel_->native_handle()) { attached_ = false; return; }
        host->detach_native_child_view(panel_->native_handle());
        attached_ = false;
    }

private:
    std::unique_ptr<view::WebViewPanel> panel_;
    SetParam set_param_;
    SetPrompt set_prompt_;
    bool attached_ = false;
};

class AccompanistRoot final : public view::View {
public:
    AccompanistRoot(SetParam set_param, SetPrompt set_prompt) {
        set_theme(view::Theme::dark());
        auto pane = std::make_unique<AccompanistPane>(std::move(set_param), std::move(set_prompt));
        pane_ = pane.get();
        add_child(std::move(pane));
    }
    AccompanistPane& pane() { return *pane_; }
    void on_resized() override {
        if (pane_) pane_->set_bounds({0, 0, bounds().width, bounds().height});
    }
private:
    AccompanistPane* pane_ = nullptr;
};

} // namespace pulp::examples::accompanist
