#pragma once

// AccompanistRoot (ModelManager PR4) — the E1 editor root that gates on model
// availability. When no Magenta model is installed/active it shows the Pulp
// ModelManagerView (download mrt2_small / mrt2_base, set default, remove) instead of
// staying silent; once a model is ready it shows the native editor plus a small
// active-model indicator (tappable to reopen the manager). Downloads run on a worker
// thread; the Processor calls poll() on its UI timer to apply progress + completion
// (no cross-thread view mutation).

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/model_manager_view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/runtime/model_store.hpp>
#include <pulp/runtime/model_download.hpp>
#include <pulp/runtime/async_stream.hpp>

#include "accompanist_native_ui.hpp"
#include "magenta_models.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace magenta_demo {

namespace acc = pulp::examples::accompanist;

inline constexpr char kMagentaSubsystem[] = "magenta";

class AccompanistRoot : public pulp::view::View {
public:
    AccompanistRoot(acc::SetParamNorm set_p, acc::GetParamNorm get_p, acc::FmtParam fmt,
                    acc::SetPrompt set_prompt, std::function<void()> on_model_changed,
                    std::string prompt)
        : set_p_(std::move(set_p)),
          get_p_(std::move(get_p)),
          fmt_(std::move(fmt)),
          set_prompt_(std::move(set_prompt)),
          on_model_changed_(std::move(on_model_changed)),
          prompt_(std::move(prompt)) {
        flex().direction = pulp::view::FlexDirection::column;
        flex().flex_grow = 1.0f;
        rebuild();
    }

    ~AccompanistRoot() override {
        if (frame_sub_ >= 0)
            if (auto* fc = frame_clock()) fc->unsubscribe(frame_sub_);
        cancel_.cancel();
        if (worker_.joinable()) worker_.detach();
    }

    /// Frame-clock tick (UI thread — safe to mutate the view tree): applies download
    /// progress + completion/cancel. Returns true to keep receiving ticks (while a
    /// download is in flight); false auto-unsubscribes when nothing is downloading.
    bool tick(float /*dt*/) {
        if (!downloading_.load(std::memory_order_acquire)) {
            frame_sub_ = -1;
            return false;
        }
        if (done_.load(std::memory_order_acquire)) {
            downloading_.store(false, std::memory_order_release);
            if (worker_.joinable()) worker_.join();
            const bool ok = success_.load();
            if (manager_) manager_->set_download_progress(active_dl_id_, -1.0f);  // clear the row
            if (ok) {
                // First model downloaded → auto-set as default + open the editor.
                // Subsequent downloads keep the current default and stay in the manager
                // (the user picks "Set default" if they want to switch).
                const bool had_active = !pulp::runtime::read_active_model_id(kMagentaSubsystem).empty();
                if (!had_active) {
                    pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, active_dl_id_);
                    if (on_model_changed_) on_model_changed_();
                    show_settings_ = false;  // first model ready → leave Settings for the editor
                }
            }
            // Cancel/failure both KEEP the .part — the row reappears as "Paused" with
            // Resume + Delete (the user chooses); a retry resumes from where it left off.
            last_pct_ = -1;
            rebuild();  // success → editor; cancel/fail → manager with the row reset
            frame_sub_ = -1;
            return false;
        }
        // Throttle: only rebuild the row when the integer percent changes.
        const int pct = static_cast<int>(progress_.load() * 100.0f + 0.5f);
        if (pct != last_pct_) {
            last_pct_ = pct;
            if (manager_) manager_->set_download_progress(active_dl_id_, progress_.load());
        }
        return true;
    }

private:
    static bool model_ready() {
        const auto id = pulp::runtime::read_active_model_id(kMagentaSubsystem);
        if (id.empty()) return false;
        return pulp::runtime::read_installed_model(kMagentaSubsystem, id).loadable();
    }

    void clear_children_all() {
        while (child_count() > 0) remove_child(child_at(0));
    }

    void rebuild() {
        // Preserve the editor's native UI (and its TextEditor's typed text) across swaps:
        // stash it out of the tree rather than destroying + recreating it.
        if (native_ui_ptr_) {
            native_ui_held_ = remove_child(native_ui_ptr_);
            native_ui_ptr_ = nullptr;
        }
        clear_children_all();  // removes the chrome and/or the settings
        manager_ = nullptr;
        if (show_settings_)
            show_settings();
        else if (model_ready())
            show_editor();
        else
            show_need_model();
    }

    void open_settings(int tab) { settings_tab_ = tab; show_settings_ = true; rebuild(); }
    void close_settings() { show_settings_ = false; rebuild(); }

    void refresh_manager_list() {
        if (manager_) manager_->set_models(pulp::runtime::list_models(magenta_models(), kMagentaSubsystem));
    }

    using V = pulp::view::View;
    using TB = pulp::view::ToggleButton;
    using Lbl = pulp::view::Label;
    static pulp::canvas::Color col(int r, int g, int b) { return pulp::canvas::Color::rgba8(r, g, b, 255); }

    // The Models settings section. No Done — the Settings header's Close handles exit.
    std::unique_ptr<pulp::view::ModelManagerView> build_models_view() {
        auto mgr = std::make_unique<pulp::view::ModelManagerView>();
        mgr->on_download = [this](const std::string& id) { start_download(id); };
        mgr->on_activate = [this](const std::string& id) {
            pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, id);
            if (on_model_changed_) on_model_changed_();
            refresh_manager_list();  // stay in Settings
        };
        mgr->on_remove = [this](const std::string& id) {
            std::string err;
            pulp::runtime::remove_model(kMagentaSubsystem, id, err);
            if (on_model_changed_) on_model_changed_();
            refresh_manager_list();
        };
        mgr->on_cancel = [this](const std::string&) { cancel_.cancel(); };
        manager_ = mgr.get();
        mgr->set_models(pulp::runtime::list_models(magenta_models(), kMagentaSubsystem));
        mgr->set_can_close(false);  // unified Settings owns the Close
        if (downloading_.load()) mgr->set_download_progress(active_dl_id_, progress_.load());
        return mgr;
    }

    std::unique_ptr<TB> make_tab(const std::string& label, int tab) {
        auto b = std::make_unique<TB>();
        b->set_label(label);
        b->set_on(settings_tab_ == tab);
        b->flex().preferred_width = 92.0f;
        b->flex().preferred_height = 28.0f;
        b->on_toggle = [this, tab](bool) { settings_tab_ = tab; rebuild(); };
        return b;
    }

    void show_settings() {
        // Header: title + Close.
        auto header = std::make_unique<V>();
        header->flex().direction = pulp::view::FlexDirection::row;
        header->flex().align_items = pulp::view::FlexAlign::center;
        header->flex().padding = 16.0f;
        header->flex().gap = 10.0f;
        auto title = std::make_unique<Lbl>("Settings");
        title->set_font_size(20.0f);
        title->set_font_weight(700);
        title->set_text_color(col(235, 235, 240));
        title->flex().flex_grow = 1.0f;
        header->add_child(std::move(title));
        auto close = std::make_unique<TB>();
        close->set_label("Close");
        close->flex().preferred_width = 88.0f;
        close->flex().preferred_height = 28.0f;
        close->on_toggle = [this](bool) { close_settings(); };
        header->add_child(std::move(close));
        add_child(std::move(header));

        // Tab bar.
        auto tabs = std::make_unique<V>();
        tabs->flex().direction = pulp::view::FlexDirection::row;
        tabs->flex().gap = 8.0f;
        tabs->flex().padding = 8.0f;
        tabs->add_child(make_tab("Models", 0));
        tabs->add_child(make_tab("Audio", 1));
        tabs->add_child(make_tab("MIDI", 2));
        add_child(std::move(tabs));

        // Content.
        if (settings_tab_ == 0) {
            add_child(build_models_view());
        } else {
            auto msg = std::make_unique<Lbl>(
                settings_tab_ == 1 ? "Audio output device / sample rate / buffer — wiring the host"
                                     " device picker next."
                                   : "MIDI input device — wiring next.");
            msg->set_font_size(13.0f);
            msg->set_text_color(col(170, 170, 175));
            msg->flex().padding = 20.0f;
            add_child(std::move(msg));
        }
    }

    void show_need_model() {
        auto wrap = std::make_unique<V>();
        wrap->flex().direction = pulp::view::FlexDirection::column;
        wrap->flex().flex_grow = 1.0f;
        wrap->flex().align_items = pulp::view::FlexAlign::center;
        wrap->flex().gap = 14.0f;
        wrap->flex().padding = 60.0f;
        auto msg = std::make_unique<Lbl>("You need to download a model to start generating.");
        msg->set_font_size(15.0f);
        msg->set_text_color(col(220, 220, 225));
        wrap->add_child(std::move(msg));
        auto btn = std::make_unique<TB>();
        btn->set_label("Download a model");
        btn->flex().preferred_width = 180.0f;
        btn->flex().preferred_height = 34.0f;
        btn->on_toggle = [this](bool) { open_settings(0); };
        wrap->add_child(std::move(btn));
        add_child(std::move(wrap));
    }

    void show_editor() {
        // Top bar: a gear that opens Settings (no model indicator).
        auto bar = std::make_unique<V>();
        bar->flex().direction = pulp::view::FlexDirection::row;
        bar->flex().padding = 10.0f;
        bar->flex().align_items = pulp::view::FlexAlign::center;
        auto spacer = std::make_unique<V>();
        spacer->flex().flex_grow = 1.0f;
        bar->add_child(std::move(spacer));
        auto gear = std::make_unique<TB>();
        gear->set_label("⚙ Settings");
        gear->flex().preferred_width = 112.0f;
        gear->flex().preferred_height = 26.0f;
        gear->on_toggle = [this](bool) { open_settings(0); };
        bar->add_child(std::move(gear));
        add_child(std::move(bar));

        // The native editor UI is built ONCE and kept alive across swaps, so the typed
        // prompt (and any in-field edit state) survives going into Settings and back.
        if (!native_ui_held_) {
            auto set_prompt_persist = [this](const std::string& p) {
                prompt_ = p;
                if (set_prompt_) set_prompt_(p);
            };
            native_ui_held_ = acc::make_accompanist_native_view(set_p_, get_p_, fmt_,
                                                                std::move(set_prompt_persist), prompt_);
        }
        native_ui_ptr_ = native_ui_held_.get();
        add_child(std::move(native_ui_held_));
    }

    void start_download(const std::string& id) {
        if (downloading_.exchange(true)) return;  // one at a time
        const auto* entry = pulp::runtime::find_model(magenta_models(), id);
        if (!entry) {
            downloading_.store(false);
            return;
        }
        active_dl_id_ = id;
        progress_.store(0.0f);
        done_.store(false);
        success_.store(false);
        cancel_ = pulp::runtime::CancellationToken{};
        if (manager_) manager_->set_download_progress(id, 0.0f);

        const auto entry_copy = *entry;
        worker_ = std::thread([this, entry_copy] {
            auto res = pulp::runtime::install_model(
                entry_copy, kMagentaSubsystem,
                [this](const pulp::runtime::DownloadProgress& p) {
                    if (p.total) progress_.store(static_cast<float>(p.downloaded) / static_cast<float>(p.total));
                    return !cancel_.is_cancelled();
                },
                &cancel_);
            success_.store(res.ok, std::memory_order_release);
            done_.store(true, std::memory_order_release);
        });

        // Drive progress/completion from the UI-thread frame clock (keeps the host's
        // 60 Hz animation timer alive while a download is in flight).
        last_pct_ = -1;
        if (auto* fc = frame_clock(); fc && frame_sub_ < 0)
            frame_sub_ = fc->subscribe([this](float dt) { return tick(dt); });
    }

    acc::SetParamNorm set_p_;
    acc::GetParamNorm get_p_;
    acc::FmtParam fmt_;
    acc::SetPrompt set_prompt_;
    std::function<void()> on_model_changed_;
    std::string prompt_;

    pulp::view::ModelManagerView* manager_ = nullptr;
    std::unique_ptr<pulp::view::View> native_ui_held_;  // editor UI when stashed (not mounted)
    pulp::view::View* native_ui_ptr_ = nullptr;          // editor UI when mounted in the tree
    bool show_settings_ = false;  // Settings overlay open
    int settings_tab_ = 0;        // 0 = Models, 1 = Audio, 2 = MIDI

    std::thread worker_;
    pulp::runtime::CancellationToken cancel_;
    std::atomic<bool> downloading_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> success_{false};
    std::atomic<float> progress_{0.0f};
    std::string active_dl_id_;
    int frame_sub_ = -1;  // FrameClock subscription id while downloading
    int last_pct_ = -1;   // throttle row rebuilds to integer-percent changes
};

inline std::unique_ptr<pulp::view::View> make_accompanist_root(
    acc::SetParamNorm set_p, acc::GetParamNorm get_p, acc::FmtParam fmt, acc::SetPrompt set_prompt,
    std::function<void()> on_model_changed, std::string prompt) {
    return std::make_unique<AccompanistRoot>(std::move(set_p), std::move(get_p), std::move(fmt),
                                             std::move(set_prompt), std::move(on_model_changed),
                                             std::move(prompt));
}

}  // namespace magenta_demo
