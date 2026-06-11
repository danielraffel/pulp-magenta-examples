#pragma once

// AccompanistRoot — V2's editor view. The native faders + prompt when a model is available,
// or a "you need a model" gate when not. A gear button reaches model management:
//   • Standalone: switches the host's card-stack chrome to its unified Settings tab
//     (Audio/MIDI device pickers + the plugin's Models tab).
//   • In a DAW (no host chrome): opens an in-EDITOR Models overlay so a plugin-only install
//     can still download a model — the host owns audio/MIDI, but model download is the
//     plugin's job. The model is shared (~/.pulp/<subsystem>), so it's downloaded once.
// The Processor calls refresh() when the active model changes.

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/buttons.hpp>       // TextButton — momentary gear / Done buttons
#include <pulp/view/ui_components.hpp>  // TabPanel — host Settings switch (standalone)
#include <pulp/view/frame_clock.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/canvas/canvas.hpp>

#include "accompanist_native_ui.hpp"
#include "model_section.hpp"  // the in-editor Models overlay (DAW)

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace magenta_demo {

namespace acc = pulp::examples::accompanist_v2;

class AccompanistRoot : public pulp::view::View {
public:
    AccompanistRoot(acc::SetParamNorm set_p, acc::GetParamNorm get_p, acc::FmtParam fmt,
                    acc::SetPrompt set_prompt, acc::StartFrozenDrag start_frozen_drag,
                    std::function<bool()> model_ready,
                    std::function<std::string()> runtime_status,
                    std::function<void()> on_model_changed, std::string prompt)
        : set_p_(std::move(set_p)),
          get_p_(std::move(get_p)),
          fmt_(std::move(fmt)),
          set_prompt_(std::move(set_prompt)),
          start_frozen_drag_(std::move(start_frozen_drag)),
          model_ready_(std::move(model_ready)),
          runtime_status_(std::move(runtime_status)),
          on_model_changed_(std::move(on_model_changed)),
          prompt_(std::move(prompt)) {
        flex().direction = pulp::view::FlexDirection::column;
        flex().flex_grow = 1.0f;
        rebuild();
    }

    ~AccompanistRoot() override { stop_status_poll(); }

    /// Called by the Processor when the active model changes so the editor swaps between the
    /// gate and the instrument.
    void refresh() { rebuild(); }

    void on_attached() override { ensure_status_poll_started(); }
    void on_detached() override { stop_status_poll(); }
    void layout_children() override {
        ensure_status_poll_started();
        pulp::view::View::layout_children();
    }

private:
    static pulp::format::SettingsPanel* find_settings_panel(pulp::view::View& view) {
        if (auto* settings = dynamic_cast<pulp::format::SettingsPanel*>(&view)) return settings;
        for (std::size_t i = 0; i < view.child_count(); ++i) {
            if (auto* child = view.child_at(i))
                if (auto* settings = find_settings_panel(*child)) return settings;
        }
        return nullptr;
    }

    static bool status_targets_models(std::string_view status) {
        return status.find("Settings > Models") != std::string_view::npos;
    }

    static bool transient_editor_status(std::string_view status) {
        return status == "Loading Magenta model..." ||
               status == "Model loaded; warming up generated audio...";
    }

    bool model_ready_now() const {
        return model_ready_ && model_ready_();
    }

    std::string runtime_status_now() const {
        return runtime_status_ ? runtime_status_() : std::string();
    }

    bool poll_status_once() {
        const bool ready = model_ready_now();
        const std::string status = runtime_status_now();
        if (ready == last_model_ready_ && status == last_runtime_status_) return false;
        last_model_ready_ = ready;
        last_runtime_status_ = status;
        rebuild();
        request_repaint();
        return true;
    }

    void ensure_status_poll_started() {
        if (frame_sub_ >= 0) return;
        if (auto* fc = frame_clock()) {
            status_clock_ = fc;
            frame_sub_ = status_clock_->subscribe([this](float) {
                poll_status_once();
                return true;
            });
            poll_status_once();
            request_repaint();
        }
    }

    void stop_status_poll() {
        if (frame_sub_ < 0) return;
        if (status_clock_) status_clock_->unsubscribe(frame_sub_);
        frame_sub_ = -1;
        status_clock_ = nullptr;
    }

    // Reach model management. In the standalone the editor lives inside a host card-stack
    // TabPanel (tab 1 = the unified Settings); switch to it. In a DAW there's no such
    // ancestor, so fall back to an in-editor Models overlay.
    void open_settings(std::string_view settings_tab = {}) {
        for (pulp::view::View* v = parent(); v != nullptr; v = v->parent())
            if (auto* tabs = dynamic_cast<pulp::view::TabPanel*>(v)) {
                for (std::size_t i = 0; i < tabs->child_count(); ++i) {
                    auto* child = tabs->child_at(i);
                    if (!child) continue;
                    if (auto* settings = find_settings_panel(*child)) {
                        if (!settings_tab.empty()) settings->set_active_tab(settings_tab);
                        tabs->set_active_tab(static_cast<int>(i));
                        return;
                    }
                }
                if (settings_tab.empty() && tabs->tab_count() > 1) {
                    tabs->set_active_tab(1);
                    return;
                }
            }
        show_models_ = true;  // DAW: no host chrome — show Models inside the plugin
        rebuild();
    }

    std::unique_ptr<pulp::view::View> make_text_button(const std::string& label,
                                                       std::function<void()> on_click) {
        auto b = std::make_unique<pulp::view::TextButton>(label);
        b->flex().preferred_width = 112.0f;
        b->flex().preferred_height = 28.0f;
        b->on_click = std::move(on_click);
        return b;
    }

    class DownloadModelButton : public pulp::view::View {
    public:
        explicit DownloadModelButton(std::function<void()> click)
            : on_click_(std::move(click)) {
            set_access_role(AccessRole::toggle);
            set_access_label("Download model");
            set_focusable(true);
            flex().preferred_width = 188.0f;
            flex().preferred_height = 36.0f;
            flex().flex_shrink = 0.0f;
        }

        void paint(pulp::canvas::Canvas& canvas) override {
            auto bg = hovered_
                ? pulp::canvas::Color::rgba8(150, 194, 255, 255)
                : pulp::canvas::Color::rgba8(137, 180, 250, 255);
            canvas.set_fill_color(bg);
            canvas.fill_rounded_rect(0, 0, bounds().width, bounds().height, 8.0f);
            canvas.set_stroke_color(pulp::canvas::Color::rgba8(185, 210, 255, 210));
            canvas.set_line_width(1.0f);
            canvas.stroke_rounded_rect(0.5f, 0.5f, bounds().width - 1.0f,
                                       bounds().height - 1.0f, 8.0f);

            canvas.set_font("system", 14.0f);
            canvas.set_fill_color(pulp::canvas::Color::rgba8(24, 27, 39, 255));
            const std::string label = "Download model";
            const float text_w = canvas.measure_text(label);
            canvas.fill_text(label, (bounds().width - text_w) * 0.5f, bounds().height * 0.65f);
        }

        void on_mouse_down(pulp::view::Point) override {
            if (on_click_) on_click_();
        }

        void on_mouse_enter() override {
            hovered_ = true;
            request_repaint();
        }

        void on_mouse_leave() override {
            hovered_ = false;
            request_repaint();
        }

    private:
        std::function<void()> on_click_;
        bool hovered_ = false;
    };

    std::unique_ptr<pulp::view::View> make_download_model_button() {
        return std::make_unique<DownloadModelButton>(
            [this] { open_settings("Models"); });
    }

    std::unique_ptr<pulp::view::View> make_status_banner(const std::string& message) {
        auto banner = std::make_unique<pulp::view::View>();
        banner->flex().direction = pulp::view::FlexDirection::row;
        banner->flex().align_items = pulp::view::FlexAlign::center;
        banner->flex().padding = 8.0f;
        banner->flex().preferred_height = 34.0f;
        banner->flex().flex_shrink = 0.0f;
        banner->set_background_color(pulp::canvas::Color::rgba8(69, 71, 90, 255));

        auto label = std::make_unique<pulp::view::Label>(message);
        label->set_font_size(12.0f);
        label->set_text_color(pulp::canvas::Color::rgba8(245, 194, 231, 255));
        banner->add_child(std::move(label));
        return banner;
    }

    // A fixed-height top bar with a single right-aligned button (gear or Done). Keeping the
    // height/padding identical means the button never jumps when views swap.
    std::unique_ptr<pulp::view::View> make_top_bar(std::unique_ptr<pulp::view::View> button) {
        auto bar = std::make_unique<pulp::view::View>();
        bar->flex().direction = pulp::view::FlexDirection::row;
        bar->flex().padding = 12.0f;
        bar->flex().preferred_height = 52.0f;
        bar->flex().flex_shrink = 0.0f;
        bar->flex().align_items = pulp::view::FlexAlign::center;
        auto spacer = std::make_unique<pulp::view::View>();
        spacer->flex().flex_grow = 1.0f;
        bar->add_child(std::move(spacer));
        bar->add_child(std::move(button));
        return bar;
    }

    void rebuild() {
        // Preserve the native UI (and its typed prompt) across swaps — stash, don't destroy.
        if (native_ui_ptr_) {
            native_ui_held_ = remove_child(native_ui_ptr_);
            native_ui_ptr_ = nullptr;
        }
        while (child_count() > 0) remove_child(child_at(0));

        const bool ready = model_ready_now();
        last_model_ready_ = ready;

        if (show_models_)
            show_models_overlay();
        else if (ready)
            show_editor();
        else
            show_need_model();
    }

    void show_editor() {
        add_child(make_top_bar(make_text_button("\xE2\x9A\x99 Settings", [this] {
            const std::string status = runtime_status_now();
            open_settings(status_targets_models(status) ? "Models" : "");
        })));
        const std::string status = runtime_status_now();
        last_runtime_status_ = status;
        if (!status.empty() && !transient_editor_status(status))
            add_child(make_status_banner(status));

        if (!native_ui_held_) {
            auto set_prompt_persist = [this](const std::string& p) {
                prompt_ = p;
                if (set_prompt_) set_prompt_(p);
            };
            native_ui_held_ = acc::make_accompanist_native_view(set_p_, get_p_, fmt_,
                                                                std::move(set_prompt_persist),
                                                                start_frozen_drag_,
                                                                prompt_);
        }
        native_ui_ptr_ = native_ui_held_.get();
        add_child(std::move(native_ui_held_));
    }

    void show_need_model() {
        const std::string status = runtime_status_now();
        last_runtime_status_ = status;
        if (!status.empty()) add_child(make_status_banner(status));

        auto wrap = std::make_unique<pulp::view::View>();
        wrap->flex().direction = pulp::view::FlexDirection::column;
        wrap->flex().flex_grow = 1.0f;
        wrap->flex().align_items = pulp::view::FlexAlign::center;
        wrap->flex().gap = 12.0f;
        wrap->flex().padding = 60.0f;

        auto title = std::make_unique<pulp::view::Label>("You need to download a model to start generating audio.");
        title->set_font_size(17.0f);
        title->set_text_color(pulp::canvas::Color::rgba8(225, 225, 230, 255));
        wrap->add_child(std::move(title));

        auto hint = std::make_unique<pulp::view::Label>(
            "Models are downloaded from Google and stored on this Mac.");
        hint->set_font_size(13.0f);
        hint->set_text_color(pulp::canvas::Color::rgba8(165, 165, 170, 255));
        wrap->add_child(std::move(hint));

        wrap->add_child(make_download_model_button());
        add_child(std::move(wrap));
    }

    // In-editor Models overlay (DAW): a Done bar + the shared ModelSection.
    void show_models_overlay() {
        add_child(make_top_bar(make_text_button("Done", [this] {
            show_models_ = false;
            rebuild();
        })));
        add_child(std::make_unique<ModelSection>([this] {
            if (on_model_changed_) on_model_changed_();  // reload the engine with the new model
        }));
    }

    acc::SetParamNorm set_p_;
    acc::GetParamNorm get_p_;
    acc::FmtParam fmt_;
    acc::SetPrompt set_prompt_;
    acc::StartFrozenDrag start_frozen_drag_;
    std::function<bool()> model_ready_;
    std::function<std::string()> runtime_status_;
    std::function<void()> on_model_changed_;
    std::string prompt_;
    bool show_models_ = false;
    int frame_sub_ = -1;
    pulp::view::FrameClock* status_clock_ = nullptr;
    bool last_model_ready_ = false;
    std::string last_runtime_status_;

    std::unique_ptr<pulp::view::View> native_ui_held_;  // stashed when not mounted
    pulp::view::View* native_ui_ptr_ = nullptr;
};

}  // namespace magenta_demo
