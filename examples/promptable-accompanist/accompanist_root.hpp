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
                pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, active_dl_id_);
                if (on_model_changed_) on_model_changed_();
                force_manager_ = false;
            }
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
        clear_children_all();
        manager_ = nullptr;
        if (model_ready() && !force_manager_)
            show_editor();
        else
            show_manager();
    }

    void refresh_manager_list() {
        if (manager_) manager_->set_models(pulp::runtime::list_models(magenta_models(), kMagentaSubsystem));
    }

    void show_manager() {
        auto mgr = std::make_unique<pulp::view::ModelManagerView>();
        mgr->set_models(pulp::runtime::list_models(magenta_models(), kMagentaSubsystem));
        mgr->on_download = [this](const std::string& id) { start_download(id); };
        mgr->on_activate = [this](const std::string& id) {
            pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, id);
            if (on_model_changed_) on_model_changed_();
            force_manager_ = false;
            rebuild();
        };
        mgr->on_remove = [this](const std::string& id) {
            std::string err;
            pulp::runtime::remove_model(kMagentaSubsystem, id, err);
            if (on_model_changed_) on_model_changed_();
            refresh_manager_list();
        };
        mgr->on_cancel = [this](const std::string&) { cancel_.cancel(); };
        manager_ = mgr.get();
        add_child(std::move(mgr));
        if (downloading_.load()) manager_->set_download_progress(active_dl_id_, progress_.load());
    }

    void show_editor() {
        // Active-model indicator (outside the manager) — tap to reopen the manager.
        auto bar = std::make_unique<pulp::view::View>();
        bar->flex().direction = pulp::view::FlexDirection::row;
        bar->flex().padding = 12.0f;
        bar->flex().gap = 8.0f;
        bar->flex().align_items = pulp::view::FlexAlign::center;

        const auto id = pulp::runtime::read_active_model_id(kMagentaSubsystem);
        auto indicator = std::make_unique<pulp::view::ToggleButton>();
        indicator->set_label("Model: " + id + "  ▾");  // ▾
        indicator->flex().preferred_height = 24.0f;
        indicator->on_toggle = [this](bool) {
            force_manager_ = true;
            rebuild();
        };
        bar->add_child(std::move(indicator));
        add_child(std::move(bar));

        add_child(acc::make_accompanist_native_view(set_p_, get_p_, fmt_, set_prompt_, prompt_));
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
    bool force_manager_ = false;

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
