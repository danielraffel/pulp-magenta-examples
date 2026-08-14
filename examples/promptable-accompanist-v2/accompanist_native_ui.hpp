// Promptable Accompanist V2 — GPU-native editor with freeze/loop sampler controls.
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

#include "accompanist_params.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::examples::accompanist_v2 {

using SetParamNorm = std::function<void(std::uint32_t, float)>;        // normalized 0..1
using GetParamNorm = std::function<float(std::uint32_t)>;
using FmtParam     = std::function<std::string(std::uint32_t)>;       // formatted actual value
using SetPrompt    = std::function<void(const std::string&)>;         // publishes desired prompt
using StartFrozenDrag = std::function<bool(view::View&, view::Point)>;
using BeginGesture = std::function<void(std::uint32_t)>;  // host write-pass arm (Touch)
using EndGesture   = std::function<void(std::uint32_t)>;  // host write-pass disarm (Release)

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
                          std::string prompt,
                          std::unique_ptr<view::View> header_accessory = nullptr,
                          BeginGesture begin_p = {}, EndGesture end_p = {})
        : get_p_(get_p), fmt_(fmt),
          begin_p_(std::move(begin_p)), end_p_(std::move(end_p)) {
        set_theme(view::Theme::dark());
        set_background_color(mag::grey900());
        flex().direction = view::FlexDirection::column;
        flex().padding = 24;
        flex().gap = 10;

        // Header row: title + subtitle on the left, an optional accessory (the
        // host Settings/Done button) on the right — top-aligned with the title
        // so it shares the first text row instead of living in its own band.
        auto header = std::make_unique<view::View>();
        header->flex().direction = view::FlexDirection::row;
        header->flex().align_items = view::FlexAlign::start;
        header->flex().gap = 12;

        auto title_block = std::make_unique<view::View>();
        title_block->flex().direction = view::FlexDirection::column;
        title_block->flex().flex_grow = 1.0f;
        title_block->flex().gap = 2;

        auto title = std::make_unique<view::Label>("Promptable Accompanist V2");
        title->set_font_size(20);
        title->set_font_weight(700);
        title->set_text_color(mag::text());
        title->flex().preferred_height = 26;
        title_block->add_child(std::move(title));

        auto sub = std::make_unique<view::Label>(
            "Magenta RealTime 2  ·  freeze and loop sampler");
        sub->set_font_size(12);
        sub->set_text_color(mag::subtext());
        sub->flex().preferred_height = 16;
        title_block->add_child(std::move(sub));

        header->add_child(std::move(title_block));
        if (header_accessory) header->add_child(std::move(header_accessory));
        add_child(std::move(header));

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
        auto* freeze_ptr = freeze.get();
        if (get_p) freeze->set_on(get_p(kFreeze) >= 0.5f);
        freeze->on_toggle = [this, set_p](bool on) {
            // A flip is a complete one-shot gesture so the DAW records the
            // discrete on/off change (mirrors bind_parameter(ToggleButton)).
            if (begin_p_) begin_p_(kFreeze);
            if (set_p) set_p(kFreeze, on ? 1.0f : 0.0f);
            if (end_p_) end_p_(kFreeze);
        };
        freeze_btn_ = freeze_ptr;
        freeze_last_on_ = get_p ? (get_p(kFreeze) >= 0.5f) : false;
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
        // Bracket the drag in a host gesture so the DAW records (and on release
        // ends) a write pass — the equivalent of bind_parameter's
        // on_gesture_begin/end. Without this, moving the fader changes the value
        // audibly but Logic's Touch mode never arms.
        const std::uint32_t gid = row_info.id;
        fader_ptr->on_gesture_begin = [this, gid] { if (begin_p_) begin_p_(gid); };
        fader_ptr->on_gesture_end   = [this, gid] { if (end_p_) end_p_(gid); };

        const float initial = get_p ? get_p(row_info.id) : 0.0f;
        faders_.push_back(BoundFader{row_info.id, fader_ptr, val_ptr, initial});
        add_child(std::move(row));
    }

public:
    // Reverse-sync: pull current parameter values into the faders so host
    // automation playback (and preset loads) move the on-screen controls.
    // Driven from AccompanistRoot's FrameClock tick. Idempotent and cheap —
    // a per-fader last-value guard skips untouched controls entirely, and
    // Fader::set_value early-returns on no change, so a static UI does no work.
    void refresh_param_displays() {
        if (!get_p_) return;
        for (auto& f : faders_) {
            const float v = get_p_(f.id);
            if (std::abs(v - f.last_norm) < 1e-4f) continue;
            f.last_norm = v;
            f.fader->set_value(v);
            if (fmt_ && f.value_label) f.value_label->set_text(fmt_(f.id));
        }
        if (freeze_btn_) {
            const bool on = get_p_(kFreeze) >= 0.5f;
            if (on != freeze_last_on_) {
                freeze_last_on_ = on;
                freeze_btn_->set_on(on);  // set_on does not re-fire on_toggle
            }
        }
    }

private:
    // Reverse-sync state: how to read parameter values + format them, and the
    // host gesture callbacks the faders bracket their drags with.
    GetParamNorm get_p_;
    FmtParam fmt_;
    BeginGesture begin_p_;
    EndGesture end_p_;

    struct BoundFader {
        std::uint32_t id;
        view::Fader* fader;
        view::Label* value_label;
        float last_norm;  // last value pushed to the fader; skips no-op refreshes
    };
    std::vector<BoundFader> faders_;

    // Freeze toggle reverse-sync (kFreeze): follows host automation playback.
    view::ToggleButton* freeze_btn_ = nullptr;
    bool freeze_last_on_ = false;
};

inline std::unique_ptr<view::View> make_accompanist_native_view(
    SetParamNorm set_p, GetParamNorm get_p, FmtParam fmt, SetPrompt set_prompt,
    StartFrozenDrag start_frozen_drag, std::string prompt,
    std::unique_ptr<view::View> header_accessory = nullptr,
    BeginGesture begin_p = {}, EndGesture end_p = {}) {
    return std::make_unique<AccompanistNativeRoot>(std::move(set_p), std::move(get_p),
                                                   std::move(fmt), std::move(set_prompt),
                                                   std::move(start_frozen_drag),
                                                   std::move(prompt),
                                                   std::move(header_accessory),
                                                   std::move(begin_p), std::move(end_p));
}

}  // namespace pulp::examples::accompanist_v2
