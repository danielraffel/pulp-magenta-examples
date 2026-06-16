#pragma once

// ModelSection — the "Models" settings tab E1 contributes to the host's unified Settings
// via Processor::settings_sections(). The host composes it alongside its own Audio/MIDI
// device tabs, so device selection stays a host concern while the plugin surfaces its own
// model management. Owns the download/store logic + the ModelManagerView. Progress runs on
// a worker thread; updates are applied on the UI thread via the host's frame clock.

#include <pulp/view/view.hpp>
#include <pulp/view/model_manager_view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/file_browser.hpp>
#include <pulp/runtime/model_store.hpp>
#include <pulp/runtime/model_download.hpp>
#include <pulp/runtime/async_stream.hpp>

#include "magenta_models.hpp"
#include "magenta_resources.hpp"  // shared resources download (Gap 2)

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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
    static pulp::runtime::ModelListResult list_magenta_models() {
        auto result = pulp::runtime::list_models(magenta_models(), kMagentaSubsystem);
        const bool resources_ok = runtime_resources_available();
        for (auto& listed : result.models) {
            if (listed.status != "installed") continue;
            if (!resources_ok ||
                !magenta_model_bundle_complete(listed.resolved_checkpoint_path)) {
                // Older one-file installs, interrupted resource downloads, or copied metadata
                // are not loadable by MRT2. Surface them as resumable repairs instead of
                // showing "Installed" and letting the engine fail at runtime.
                listed.status = "partial";
                listed.partial_fraction = 0.0f;
            }
        }
        return result;
    }

    static bool active_model_complete(const std::string& id) {
        if (id.empty()) return false;
        const auto rec = pulp::runtime::read_installed_model(kMagentaSubsystem, id);
        return rec.metadata_found && runtime_resources_available() &&
               magenta_model_bundle_complete(rec.resolved_checkpoint_path);
    }

    static constexpr pulp::canvas::Color kMuted() {
        return pulp::canvas::Color::rgba8(170, 170, 175, 255);
    }

    static constexpr pulp::canvas::Color kWhite() {
        return pulp::canvas::Color::rgba8(235, 235, 240, 255);
    }

    static constexpr pulp::canvas::Color kPanel() {
        return pulp::canvas::Color::rgba8(32, 33, 36, 255);
    }

    static std::unique_ptr<pulp::view::Label> make_label(const std::string& text,
                                                         float size,
                                                         pulp::canvas::Color color) {
        auto label = std::make_unique<pulp::view::Label>(text);
        label->set_font_size(size);
        label->set_text_color(color);
        return label;
    }

    // Shown when a download fails for a reason other than the user cancelling.
    // Names the sandbox case explicitly: an audio-unit host (e.g. Logic) can
    // block the plug-in process's network, so the standalone app / copy is the
    // way out. `what` is "the model" / "the model resources".
    static std::string network_failure_message(const char* what) {
        return std::string("Couldn't download ") + what +
               " from Google. If this plug-in is running inside a DAW, the host "
               "may be blocking its network \xE2\x80\x94 download in the standalone "
               "app, or copy ~/.pulp/magenta from another machine.";
    }

    static bool complete_model_at(const std::filesystem::path& root,
                                  const std::string& model_id) {
        return magenta_model_bundle_complete(root / model_id / (model_id + ".mlxfn"));
    }

    static std::string detected_model_summary(const std::filesystem::path& root) {
        std::vector<std::string> names;
        if (complete_model_at(root, "mrt2_small")) names.push_back("Small");
        if (complete_model_at(root, "mrt2_base")) names.push_back("Large");

        if (names.empty()) return "No model installed yet";
        if (names.size() == 1) return names.front() + " detected";
        return names[0] + " and " + names[1] + " detected";
    }

    static std::filesystem::path nearest_existing_directory(std::filesystem::path path) {
        std::error_code ec;
        if (path.empty()) return {};
        if (std::filesystem::is_regular_file(path, ec)) path = path.parent_path();
        for (;;) {
            ec.clear();
            if (std::filesystem::is_directory(path, ec)) return path;
            const auto parent = path.parent_path();
            if (parent.empty() || parent == path) return {};
            path = parent;
        }
    }

    static void open_folder(const std::filesystem::path& path) {
        if (const auto target = nearest_existing_directory(path); !target.empty())
            pulp::view::ContentSharer::share_file(target);
    }

    static std::unique_ptr<pulp::view::TextButton> make_open_button(
        const std::filesystem::path& path) {
        auto button = std::make_unique<pulp::view::TextButton>("Open Folder");
        button->flex().preferred_width = 112.0f;
        button->flex().preferred_height = 28.0f;
        button->set_enabled(!path.empty());
        button->on_click = [path] { open_folder(path); };
        return button;
    }

    static std::unique_ptr<pulp::view::View> make_storage_row(
        const std::string& title,
        const std::filesystem::path& path) {
        auto row = std::make_unique<pulp::view::View>();
        row->flex().direction = pulp::view::FlexDirection::row;
        row->flex().align_items = pulp::view::FlexAlign::center;
        row->flex().gap = 10.0f;
        row->flex().preferred_height = 48.0f;

        auto text = std::make_unique<pulp::view::View>();
        text->flex().direction = pulp::view::FlexDirection::column;
        text->flex().gap = 2.0f;
        text->flex().flex_grow = 1.0f;
        auto title_label = make_label(title + " - " + detected_model_summary(path),
                                      12.0f,
                                      kWhite());
        title_label->set_font_weight(700);
        text->add_child(std::move(title_label));
        text->add_child(make_label(path.empty() ? "(unavailable)" : path.string(), 10.0f, kMuted()));

        row->add_child(std::move(text));
        row->add_child(make_open_button(path));
        return row;
    }

    std::unique_ptr<pulp::view::View> make_storage_section() {
        auto section = std::make_unique<pulp::view::View>();
        section->flex().direction = pulp::view::FlexDirection::column;
        section->flex().gap = 8.0f;
        section->flex().padding_left = 20.0f;
        section->flex().padding_right = 20.0f;
        section->flex().padding_bottom = 20.0f;
        section->set_background_color(kPanel());

        auto heading = make_label("Model files", 14.0f, kWhite());
        heading->set_font_weight(700);
        section->add_child(std::move(heading));
        section->add_child(make_storage_row(
            "Model folder",
            pulp::runtime::resolve_pulp_home() / kMagentaSubsystem / "models"));
        return section;
    }

    void refresh_storage_section() {
        if (storage_section_) {
            remove_child(storage_section_);
            storage_section_ = nullptr;
        }
        auto storage = make_storage_section();
        storage_section_ = storage.get();
        add_child(std::move(storage));
    }

    void refresh_list() {
        if (manager_) manager_->set_models(list_magenta_models());
        refresh_storage_section();
    }

    void build() {
        while (child_count() > 0) remove_child(child_at(0));
        storage_section_ = nullptr;
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
        mgr->set_models(list_magenta_models());
        mgr->set_can_close(false);  // the host Settings panel owns navigation
        if (downloading_.load()) mgr->set_download_progress(active_dl_id_, progress_.load());
        add_child(std::move(mgr));
        // Download-failure message (empty until a non-cancel failure). The SDK
        // ModelManagerView only shows progress/Cancel, so a failed download would
        // otherwise just silently revert to "Download"; this surfaces why.
        auto err_label = make_label("", 12.0f,
                                    pulp::canvas::Color::rgba8(235, 120, 120, 255));
        error_label_ = err_label.get();
        add_child(std::move(err_label));
        refresh_storage_section();
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
                // First downloaded model auto-becomes the default. If this was repairing the
                // currently-active model, rewrite the active state too so stale metadata is
                // refreshed after old one-file installs.
                const auto active = pulp::runtime::read_active_model_id(kMagentaSubsystem);
                if (active.empty() || active == active_dl_id_ || !active_model_complete(active))
                    pulp::runtime::activate_model(magenta_models(), kMagentaSubsystem, active_dl_id_);
                if (on_model_changed_) on_model_changed_();
            } else if (error_label_) {
                // Non-empty only on a real failure (empty on user cancel), so a
                // cancelled download clears the message rather than showing one.
                error_label_->set_text(download_error_);
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
        download_error_.clear();
        if (error_label_) error_label_->set_text("");
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
            std::string err;
            if (res_total > 0) {
                ok = download_resources(
                    [this, res_weight](std::uint64_t done, std::uint64_t total) {
                        if (total)
                            progress_.store(static_cast<float>(res_weight * static_cast<double>(done) /
                                                               static_cast<double>(total)));
                        return !cancel_.is_cancelled();
                    },
                    &cancel_);
                if (!ok && !cancel_.is_cancelled())
                    err = network_failure_message("the model resources");
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
                if (!ok && !cancel_.is_cancelled())
                    err = res.error.empty()
                              ? network_failure_message("the model")
                              : ("Model download failed: " + res.error);
            }
            // Published before done_ (release) so the UI-thread reader in tick()
            // sees the message once it observes done_ (acquire). Empty on a
            // user cancel, so no error is shown for an intentional Cancel.
            download_error_ = err;
            success_.store(ok, std::memory_order_release);
            done_.store(true, std::memory_order_release);
        });

        last_pct_ = -1;
        if (auto* fc = frame_clock(); fc && frame_sub_ < 0)
            frame_sub_ = fc->subscribe([this](float dt) { return tick(dt); });
    }

    std::function<void()> on_model_changed_;
    pulp::view::ModelManagerView* manager_ = nullptr;
    pulp::view::View* storage_section_ = nullptr;
    pulp::view::Label* error_label_ = nullptr;
    std::string download_error_;  // set by worker before done_ (release); read in tick()
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
