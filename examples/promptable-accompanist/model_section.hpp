#pragma once

// ModelSection — the "Models" settings tab E1 contributes to the host's unified Settings
// via Processor::settings_sections(). The host composes it alongside its own Audio/MIDI
// device tabs, so device selection stays a host concern while the plugin surfaces its own
// model management. Owns the download/store logic + the ModelManagerView. Progress runs on
// a worker thread; updates are applied on the UI thread via the host's frame clock.

#include <pulp/view/view.hpp>
#include <pulp/view/model_manager_view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/runtime/model_store.hpp>
#include <pulp/runtime/model_download.hpp>
#include <pulp/runtime/async_stream.hpp>

#include "magenta_models.hpp"
#include "magenta_resources.hpp"  // shared resources download (Gap 2)

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace magenta_demo {

class ModelSection : public pulp::view::View {
public:
    explicit ModelSection(std::function<void()> on_model_changed)
        : on_model_changed_(std::move(on_model_changed)) {
        flex().direction = pulp::view::FlexDirection::column;
        flex().flex_grow = 1.0f;
        build();
    }

    ~ModelSection() override {
        if (frame_sub_ >= 0)
            if (auto* fc = frame_clock()) fc->unsubscribe(frame_sub_);
        cancel_.cancel();
        if (worker_.joinable()) worker_.detach();
    }

private:
    void refresh_list() {
        if (manager_) manager_->set_models(pulp::runtime::list_models(magenta_models(), kMagentaSubsystem));
    }

    void build() {
        while (child_count() > 0) remove_child(child_at(0));
        auto mgr = std::make_unique<pulp::view::ModelManagerView>();
        mgr->on_download = [this](const std::string& id) { start_download(id); };
        mgr->on_activate = [this](const std::string& id) {
            pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, id);
            if (on_model_changed_) on_model_changed_();
            refresh_list();
        };
        mgr->on_remove = [this](const std::string& id) {
            std::string err;
            pulp::runtime::remove_model(kMagentaSubsystem, id, err);
            if (on_model_changed_) on_model_changed_();
            refresh_list();
        };
        mgr->on_cancel = [this](const std::string&) { cancel_.cancel(); };
        manager_ = mgr.get();
        mgr->set_models(pulp::runtime::list_models(magenta_models(), kMagentaSubsystem));
        mgr->set_can_close(false);  // the host Settings panel owns navigation
        if (downloading_.load()) mgr->set_download_progress(active_dl_id_, progress_.load());
        add_child(std::move(mgr));
    }

    bool tick(float /*dt*/) {
        if (!downloading_.load(std::memory_order_acquire)) {
            frame_sub_ = -1;
            return false;
        }
        if (done_.load(std::memory_order_acquire)) {
            downloading_.store(false, std::memory_order_release);
            if (worker_.joinable()) worker_.join();
            if (manager_) manager_->set_download_progress(active_dl_id_, -1.0f);
            if (success_.load()) {
                // First downloaded model auto-becomes the default.
                if (pulp::runtime::read_active_model_id(kMagentaSubsystem).empty())
                    pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, active_dl_id_);
                if (on_model_changed_) on_model_changed_();
            }
            last_pct_ = -1;
            refresh_list();
            frame_sub_ = -1;
            return false;
        }
        const int pct = static_cast<int>(progress_.load() * 100.0f + 0.5f);
        if (pct != last_pct_) {
            last_pct_ = pct;
            if (manager_) manager_->set_download_progress(active_dl_id_, progress_.load());
        }
        return true;
    }

    void start_download(const std::string& id) {
        if (downloading_.exchange(true)) return;
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
            // The shared resources (~1.3 GB) are required by every model. A plugin-only
            // install won't have them, so fetch any missing ones FIRST, then the model —
            // one combined progress bar weighted by byte size so it advances smoothly
            // across both phases regardless of each phase's internal progress units.
            const std::uint64_t res_total =
                shared_resources_complete() ? 0 : magenta_resources_total_bytes();
            const std::uint64_t grand_total = res_total + entry_copy.size_bytes;
            const double res_weight = grand_total ? static_cast<double>(res_total) / grand_total : 0.0;
            const double model_weight =
                grand_total ? static_cast<double>(entry_copy.size_bytes) / grand_total : 1.0;

            bool ok = true;
            if (res_total > 0) {
                ok = download_resources(
                    [this, res_weight](std::uint64_t done, std::uint64_t total) {
                        if (total)
                            progress_.store(static_cast<float>(res_weight * static_cast<double>(done) /
                                                               static_cast<double>(total)));
                        return !cancel_.is_cancelled();
                    },
                    &cancel_);
            }
            if (ok) {
                auto res = pulp::runtime::install_model(
                    entry_copy, kMagentaSubsystem,
                    [this, res_weight, model_weight](const pulp::runtime::DownloadProgress& p) {
                        if (p.total)
                            progress_.store(static_cast<float>(
                                res_weight + model_weight * static_cast<double>(p.downloaded) /
                                                 static_cast<double>(p.total)));
                        return !cancel_.is_cancelled();
                    },
                    &cancel_);
                ok = res.ok;
            }
            success_.store(ok, std::memory_order_release);
            done_.store(true, std::memory_order_release);
        });

        last_pct_ = -1;
        if (auto* fc = frame_clock(); fc && frame_sub_ < 0)
            frame_sub_ = fc->subscribe([this](float dt) { return tick(dt); });
    }

    std::function<void()> on_model_changed_;
    pulp::view::ModelManagerView* manager_ = nullptr;
    std::thread worker_;
    pulp::runtime::CancellationToken cancel_;
    std::atomic<bool> downloading_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> success_{false};
    std::atomic<float> progress_{0.0f};
    std::string active_dl_id_;
    int frame_sub_ = -1;
    int last_pct_ = -1;
};

}  // namespace magenta_demo
