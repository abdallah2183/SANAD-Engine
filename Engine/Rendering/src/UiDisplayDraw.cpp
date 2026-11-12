// NF/Rendering/UiDisplayDraw.cpp — see UiDisplayDraw.hpp. Extracted verbatim
// from GameUiOverlay::draw so NFPlayer and the editor Game View share one
// rect/line/text path (shape-once, measured wrap, per-line alignment).

#include <NF/Rendering/UiDisplayDraw.hpp>

#include <NF/UI/ArabicShaper.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace nf::rendering {
namespace {

ImU32 to_u32(const ui::UiColor& c) {
    const auto ch = [](float v) -> unsigned {
        const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<unsigned>(clamped * 255.0f + 0.5f);
    };
    return IM_COL32(ch(c.r), ch(c.g), ch(c.b), ch(c.a));
}

} // namespace

void draw_display_list(ImDrawList* dl, const ui::DisplayList& list, ImVec2 offset,
                       ImFont* font, float default_font_size,
                       const ui::TextMeasurer& measurer) {
    if (dl == nullptr || font == nullptr) {
        return;
    }
    for (const ui::DlRect& r : list.rects) {
        dl->AddRectFilled(ImVec2(r.rect.x + offset.x, r.rect.y + offset.y),
                          ImVec2(r.rect.x + r.rect.w + offset.x, r.rect.y + r.rect.h + offset.y),
                          to_u32(r.color));
    }
    for (const ui::DlLine& l : list.lines) {
        dl->AddLine(ImVec2(l.x1 + offset.x, l.y1 + offset.y),
                    ImVec2(l.x2 + offset.x, l.y2 + offset.y), to_u32(l.color), l.width);
    }
    auto shape = [](const std::string& logical) -> std::string {
        return ui::needs_shaping(logical) ? ui::shape_arabic(logical) : logical;
    };
    auto measure = [&](const std::string& visual, float fs) -> float {
        if (measurer) return measurer(visual, fs);
        ImVec2 m = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, visual.c_str(), visual.c_str() + visual.size());
        return m.x;
    };
    for (const ui::DlText& t : list.texts) {
        if (t.text.empty()) {
            continue;
        }
        const float size = (t.size > 0.0f) ? t.size : default_font_size;
        if (t.wrap_width <= 0.0f) {
            const std::string visual = shape(t.text);
            const ImVec2 m = font->CalcTextSizeA(size, FLT_MAX, 0.0f, visual.c_str(), visual.c_str() + visual.size());
            ImVec2 at(t.x + offset.x, t.y + offset.y);
            switch (t.align) {
            case ui::TextAlign::Center: at.x -= m.x * 0.5f; break;
            case ui::TextAlign::Right: at.x -= m.x; break;
            case ui::TextAlign::Left: break;
            }
            dl->AddText(font, size, at, to_u32(t.color), visual.c_str(), visual.c_str() + visual.size());
            continue;
        }
        const float box_w = std::max(1.0f, t.wrap_width);
        const float box_h = std::max(t.box_height, size);
        const float line_h = ui::line_height_for(size);
        std::vector<std::string> visual_lines = ui::wrap_text(t.text, box_w, size, measure);
        const float block_h = static_cast<float>(visual_lines.size()) * line_h;
        float y0 = t.y + (box_h > block_h ? (box_h - block_h) * 0.5f : 0.0f) + offset.y;
        for (std::size_t li = 0; li < visual_lines.size(); ++li) {
            const std::string& vis = visual_lines[li];
            if (vis.empty()) continue;
            const ImVec2 m = font->CalcTextSizeA(size, FLT_MAX, 0.0f, vis.c_str(), vis.c_str() + vis.size());
            float lx = t.x + offset.x;
            switch (t.align) {
            case ui::TextAlign::Center: lx = t.x + offset.x + (box_w - m.x) * 0.5f; break;
            case ui::TextAlign::Right: lx = t.x + offset.x + box_w - m.x; break;
            case ui::TextAlign::Left: break;
            }
            dl->AddText(font, size, ImVec2(lx, y0), to_u32(t.color), vis.c_str(), vis.c_str() + vis.size());
            y0 += line_h;
        }
    }
}

} // namespace nf::rendering
