// Promptable Accompanist V2 — GPU-native editor with freeze/loop sampler controls.
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

#include "accompanist_params.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace pulp::examples::accompanist_v2 {

using SetParamNorm = std::function<void(std::uint32_t, float)>;        // normalized 0..1
using GetParamNorm = std::function<float(std::uint32_t)>;
using FmtParam     = std::function<std::string(std::uint32_t)>;       // formatted actual value
using SetPrompt    = std::function<void(const std::string&)>;         // publishes desired prompt
using StartFrozenDrag = std::function<bool(view::View&, view::Point)>;

// Magenta tokens (examples/common/react_ui/colors.ts).
namespace mag {
inline canvas::Color grey900()   { return canvas::Color::rgba8(0x20, 0x21, 0x24); }
inline canvas::Color panel()     { return canvas::Color::rgba8(0x2A, 0x2C, 0x31); }
inline canvas::Color lightBlue() { return canvas::Color::rgba8(0x7F, 0xB2, 0xFF); }
inline canvas::Color accentOn()  { return canvas::Color::rgba8(0x74, 0xD4, 0xD4); }
inline canvas::Color text()      { return canvas::Color::rgba8(0xEC, 0xEE, 0xF4); }
inline canvas::Color subtext()   { return canvas::Color::rgba8(0x9A, 0xA0, 0xB2); }
}  // namespace mag

class AccompanistNativeRoot final : public view::View {
    struct ParamRow {
        std::uint32_t id;
        const char* name;
        float value_width;
    };

public:
    AccompanistNativeRoot(SetParamNorm set_p, GetParamNorm get_p, FmtParam fmt,
                          SetPrompt set_prompt, StartFrozenDrag start_frozen_drag,
                          std::string prompt) {
        set_theme(view::Theme::dark());
        set_background_color(mag::grey900());
        flex().direction = view::FlexDirection::column;
        flex().padding = 24;
        flex().gap = 10;

        auto title = std::make_unique<view::Label>("Promptable Accompanist V2");
        title->set_font_size(20);
        title->set_font_weight(700);
        title->set_text_color(mag::text());
        title->flex().preferred_height = 26;
        add_child(std::move(title));

        auto sub = std::make_unique<view::Label>(
            "Magenta RealTime 2  ·  freeze and loop sampler");
        sub->set_font_size(12);
        sub->set_text_color(mag::subtext());
        sub->flex().preferred_height = 16;
        add_child(std::move(sub));

        auto prompt_box = std::make_unique<view::TextEditor>();
        prompt_box->set_text(prompt);
        prompt_box->placeholder = "describe the music...";
        prompt_box->set_font_size(14);
        prompt_box->set_background_color(mag::panel());
        prompt_box->flex().preferred_height = 44;
        prompt_box->on_change = [set_prompt](const std::string& t) {
            if (set_prompt) set_prompt(t);
        };
        add_child(std::move(prompt_box));

        add_freeze_row(set_p, get_p, std::move(start_frozen_drag));

        static constexpr ParamRow kRows[] = {
            {kTemperature,     "Temperature", 58.0f},
            {kTopK,            "Top K",       58.0f},
            {kCfgMusicCoCa,    "Prompt CFG",  58.0f},
            {kCfgNotes,        "Notes CFG",   58.0f},
            {kCfgDrums,        "Drums CFG",   58.0f},
            {kVolumeDb,        "Volume",      58.0f},
            {kCaptureSeconds,  "Capture",     68.0f},
            {kLoopCrossfadeMs, "Loop XFade",  68.0f},
        };
        for (const auto& row_info : kRows) {
            add_fader_row(row_info, set_p, get_p, fmt);
        }
    }

private:
    class FreezeDragButton final : public view::ToggleButton {
    public:
        explicit FreezeDragButton(StartFrozenDrag start_drag)
            : start_drag_(std::move(start_drag)) {}

        void on_mouse_down(view::Point pos) override {
            pressed_ = true;
            drag_attempted_ = false;
            drag_started_ = false;
            press_pos_ = pos;
        }

        void on_mouse_drag(view::Point pos) override {
            if (!pressed_ || drag_attempted_) return;
            const float dx = pos.x - press_pos_.x;
            const float dy = pos.y - press_pos_.y;
            if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) return;

            drag_attempted_ = true;
            if (start_drag_) {
                drag_started_ = start_drag_(*this, local_to_root(pos));
            }
        }

        void on_mouse_up(view::Point) override {
            if (!pressed_) return;
            const bool should_toggle = !drag_attempted_ && !drag_started_;
            pressed_ = false;
            drag_attempted_ = false;
            drag_started_ = false;
            if (!should_toggle) return;

            set_on(!is_on());
            if (on_toggle) on_toggle(is_on());
        }

        void on_mouse_cancel(view::Point) override {
            pressed_ = false;
            drag_attempted_ = false;
            drag_started_ = false;
        }

    private:
        view::Point local_to_root(view::Point local) const {
            for (const view::View* v = this; v; v = v->parent()) {
                local.x += v->bounds().x;
                local.y += v->bounds().y;
            }
            return local;
        }

        static constexpr float kDragThresholdPx = 4.0f;
        StartFrozenDrag start_drag_;
        view::Point press_pos_{};
        bool pressed_ = false;
        bool drag_attempted_ = false;
        bool drag_started_ = false;
    };

    void add_freeze_row(const SetParamNorm& set_p, const GetParamNorm& get_p,
                        StartFrozenDrag start_frozen_drag) {
        auto row = std::make_unique<view::View>();
        row->flex().direction = view::FlexDirection::row;
        row->flex().gap = 12;
        row->flex().preferred_height = 36;

        auto label = std::make_unique<view::Label>("Sampler");
        label->set_font_size(13);
        label->set_text_color(mag::subtext());
        label->flex().preferred_width = 104;
        row->add_child(std::move(label));

        auto freeze = std::make_unique<FreezeDragButton>(std::move(start_frozen_drag));
        freeze->set_label("Freeze");
        freeze->set_font_size(13.0f);
        freeze->set_corner_radius(8.0f);
        freeze->set_on_background_color(mag::accentOn());
        freeze->set_off_background_color(mag::panel());
        freeze->set_on_text_color(canvas::Color::rgba8(0x12, 0x17, 0x1B));
        freeze->set_off_text_color(mag::text());
        freeze->flex().flex_grow = 1;
        freeze->flex().preferred_height = 34;
        if (get_p) freeze->set_on(get_p(kFreeze) >= 0.5f);
        freeze->on_toggle = [set_p](bool on) {
            if (set_p) set_p(kFreeze, on ? 1.0f : 0.0f);
        };
        row->add_child(std::move(freeze));
        add_child(std::move(row));
    }

    void add_fader_row(const ParamRow& row_info, const SetParamNorm& set_p,
                       const GetParamNorm& get_p, const FmtParam& fmt) {
        auto row = std::make_unique<view::View>();
        row->flex().direction = view::FlexDirection::row;
        row->flex().gap = 12;
        row->flex().preferred_height = 30;

        auto name = std::make_unique<view::Label>(row_info.name);
        name->set_font_size(13);
        name->set_text_color(mag::subtext());
        name->flex().preferred_width = 104;
        row->add_child(std::move(name));

        auto fader = std::make_unique<view::Fader>();
        auto* fader_ptr = fader.get();
        fader->set_orientation(view::Fader::Orientation::horizontal);
        fader->flex().flex_grow = 1;
        if (get_p) fader->set_value(get_p(row_info.id));
        row->add_child(std::move(fader));

        auto val = std::make_unique<view::Label>(fmt ? fmt(row_info.id) : std::string());
        val->set_font_size(13);
        val->set_text_color(mag::lightBlue());
        val->set_text_align(view::LabelAlign::right);
        val->flex().preferred_width = row_info.value_width;
        view::Label* val_ptr = val.get();
        row->add_child(std::move(val));

        fader_ptr->on_change = [id = row_info.id, set_p, fmt, val_ptr](float v) {
            if (set_p) set_p(id, v);
            if (fmt && val_ptr) val_ptr->set_text(fmt(id));
        };
        add_child(std::move(row));
    }
};

inline std::unique_ptr<view::View> make_accompanist_native_view(
    SetParamNorm set_p, GetParamNorm get_p, FmtParam fmt, SetPrompt set_prompt,
    StartFrozenDrag start_frozen_drag, std::string prompt) {
    return std::make_unique<AccompanistNativeRoot>(std::move(set_p), std::move(get_p),
                                                   std::move(fmt), std::move(set_prompt),
                                                   std::move(start_frozen_drag),
                                                   std::move(prompt));
}

}  // namespace pulp::examples::accompanist_v2
