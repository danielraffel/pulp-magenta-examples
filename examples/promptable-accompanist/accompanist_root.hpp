#pragma once

// AccompanistRoot — E1's editor view. Just the instrument UI now: the native faders +
// prompt when a model is available, or a "you need a model" gate pointing at the host's
// Settings → Models tab when not. Model management + audio/MIDI device settings live in
// the host's unified Settings panel (Processor::settings_sections + the host SettingsPanel)
// — this view owns neither. The Processor calls refresh() when the active model changes.

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

#include "accompanist_native_ui.hpp"

#include <functional>
#include <memory>
#include <string>

namespace magenta_demo {

namespace acc = pulp::examples::accompanist;

class AccompanistRoot : public pulp::view::View {
public:
    AccompanistRoot(acc::SetParamNorm set_p, acc::GetParamNorm get_p, acc::FmtParam fmt,
                    acc::SetPrompt set_prompt, std::function<bool()> model_ready, std::string prompt)
        : set_p_(std::move(set_p)),
          get_p_(std::move(get_p)),
          fmt_(std::move(fmt)),
          set_prompt_(std::move(set_prompt)),
          model_ready_(std::move(model_ready)),
          prompt_(std::move(prompt)) {
        flex().direction = pulp::view::FlexDirection::column;
        flex().flex_grow = 1.0f;
        rebuild();
    }

    /// Called by the Processor when the active model changes (downloaded / removed in the
    /// host Settings → Models tab) so the editor swaps between the gate and the instrument.
    void refresh() { rebuild(); }

private:
    void rebuild() {
        // Preserve the native UI (and its typed prompt) across swaps — stash, don't destroy.
        if (native_ui_ptr_) {
            native_ui_held_ = remove_child(native_ui_ptr_);
            native_ui_ptr_ = nullptr;
        }
        while (child_count() > 0) remove_child(child_at(0));
        if (model_ready_ && model_ready_())
            show_editor();
        else
            show_need_model();
    }

    void show_editor() {
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

    void show_need_model() {
        auto wrap = std::make_unique<pulp::view::View>();
        wrap->flex().direction = pulp::view::FlexDirection::column;
        wrap->flex().flex_grow = 1.0f;
        wrap->flex().align_items = pulp::view::FlexAlign::center;
        wrap->flex().gap = 12.0f;
        wrap->flex().padding = 60.0f;

        auto title = std::make_unique<pulp::view::Label>("You need a model to start generating.");
        title->set_font_size(16.0f);
        title->set_text_color(pulp::canvas::Color::rgba8(225, 225, 230, 255));
        wrap->add_child(std::move(title));

        auto hint = std::make_unique<pulp::view::Label>(
            "Open the Settings tab → Models and download mrt2_small or mrt2_base.");
        hint->set_font_size(13.0f);
        hint->set_text_color(pulp::canvas::Color::rgba8(165, 165, 170, 255));
        wrap->add_child(std::move(hint));

        add_child(std::move(wrap));
    }

    acc::SetParamNorm set_p_;
    acc::GetParamNorm get_p_;
    acc::FmtParam fmt_;
    acc::SetPrompt set_prompt_;
    std::function<bool()> model_ready_;
    std::string prompt_;

    std::unique_ptr<pulp::view::View> native_ui_held_;  // stashed when not mounted
    pulp::view::View* native_ui_ptr_ = nullptr;
};

}  // namespace magenta_demo
