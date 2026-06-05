// Promptable Accompanist (E1) — GPU-native editor (Track B).
//
// Built from Pulp's native widgets (Skia-drawn into the GPU canvas) to Magenta's
// design tokens (examples/common/react_ui): GREY_900 #202124 background, the
// teal→pink accent palette, labeled sliders. Unlike the WebView track this renders
// on Pulp's own GPU and is headlessly capturable via render_to_png / capture_view.
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pulp::examples::accompanist {

using SetParamNorm = std::function<void(std::uint32_t, float)>;        // normalized 0..1
using GetParamNorm = std::function<float(std::uint32_t)>;
using FmtParam     = std::function<std::string(std::uint32_t)>;       // formatted actual value

// Magenta tokens (examples/common/react_ui/colors.ts).
namespace mag {
inline canvas::Color grey900()  { return canvas::Color::rgba8(0x20, 0x21, 0x24); }
inline canvas::Color panel()    { return canvas::Color::rgba8(0x2A, 0x2C, 0x31); }
inline canvas::Color lightBlue(){ return canvas::Color::rgba8(0x7F, 0xB2, 0xFF); }
inline canvas::Color text()     { return canvas::Color::rgba8(0xEC, 0xEE, 0xF4); }
inline canvas::Color subtext()  { return canvas::Color::rgba8(0x9A, 0xA0, 0xB2); }
}  // namespace mag

class AccompanistNativeRoot final : public view::View {
public:
    AccompanistNativeRoot(SetParamNorm set_p, GetParamNorm get_p, FmtParam fmt,
                          std::string prompt) {
        set_theme(view::Theme::dark());
        set_background_color(mag::grey900());
        flex().direction = view::FlexDirection::column;
        flex().padding = 24;
        flex().gap = 10;

        auto title = std::make_unique<view::Label>("Promptable Accompanist");
        title->set_font_size(20);
        title->set_font_weight(700);
        title->set_text_color(mag::text());
        title->flex().preferred_height = 26;
        add_child(std::move(title));

        auto sub = std::make_unique<view::Label>(
            "Magenta RealTime 2  ·  generative  ·  ~200 ms latency");
        sub->set_font_size(12);
        sub->set_text_color(mag::subtext());
        sub->flex().preferred_height = 16;
        add_child(std::move(sub));

        auto prompt_box = std::make_unique<view::Label>(
            prompt.empty() ? std::string("warm analog pads") : prompt);
        prompt_box->set_font_size(14);
        prompt_box->set_text_color(mag::text());
        prompt_box->set_background_color(mag::panel());
        prompt_box->flex().preferred_height = 44;
        add_child(std::move(prompt_box));

        static const char* kNames[6] = {"Temperature", "Top K", "Prompt CFG",
                                        "Notes CFG",   "Drums CFG", "Volume"};
        for (std::uint32_t i = 0; i < 6; ++i) {
            auto row = std::make_unique<view::View>();
            row->flex().direction = view::FlexDirection::row;
            row->flex().gap = 12;
            row->flex().preferred_height = 30;

            auto name = std::make_unique<view::Label>(kNames[i]);
            name->set_font_size(13);
            name->set_text_color(mag::subtext());
            name->flex().preferred_width = 104;
            row->add_child(std::move(name));

            auto fader = std::make_unique<view::Fader>();
            fader->set_orientation(view::Fader::Orientation::horizontal);
            fader->flex().flex_grow = 1;
            if (get_p) fader->set_value(get_p(i));
            row->add_child(std::move(fader));

            auto val = std::make_unique<view::Label>(fmt ? fmt(i) : std::string());
            val->set_font_size(13);
            val->set_text_color(mag::lightBlue());
            val->set_text_align(view::LabelAlign::right);
            val->flex().preferred_width = 52;
            view::Label* val_ptr = val.get();
            row->add_child(std::move(val));

            // Live: moving the fader updates the param + the value readout.
            if (auto* f = dynamic_cast<view::Fader*>(row->child_at(1))) {
                f->on_change = [i, set_p, fmt, val_ptr](float v) {
                    if (set_p) set_p(i, v);
                    if (fmt && val_ptr) val_ptr->set_text(fmt(i));
                };
            }
            add_child(std::move(row));
        }
    }
};

inline std::unique_ptr<view::View> make_accompanist_native_view(
    SetParamNorm set_p, GetParamNorm get_p, FmtParam fmt, std::string prompt) {
    return std::make_unique<AccompanistNativeRoot>(std::move(set_p), std::move(get_p),
                                                   std::move(fmt), std::move(prompt));
}

}  // namespace pulp::examples::accompanist
