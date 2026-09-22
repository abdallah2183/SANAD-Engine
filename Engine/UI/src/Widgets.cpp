// NF/UI/Widgets.cpp — retained-mode widget drawing (Game-Ready G3).

#include <NF/UI/Widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace nf::ui {

namespace {

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

float px(float normalized, float total) { return normalized * total; }

void push_text(DisplayList& dl, const Label& l, float width, float height) {
    DlText t;
    t.text = l.text;
    t.size = l.size;
    t.color = l.color;
    t.align = l.align;
    t.x = px(l.rect.x, width);
    t.y = px(l.rect.y, height);
    // Center/right alignment pivots on the rect's width in pixels.
    if (l.align == TextAlign::Center) t.x += px(l.rect.w, width) * 0.5f;
    if (l.align == TextAlign::Right) t.x += px(l.rect.w, width);
    dl.texts.push_back(std::move(t));
}

} // namespace

// --- MenuList ----------------------------------------------------------------

bool MenuList::move(int delta) {
    if (items.empty()) return false;
    // Bound the walk: at most items.size() steps before every eligible index
    // has been visited (all-disabled => give up unchanged).
    for (int step = 0; step < static_cast<int>(items.size()); ++step) {
        selected += delta;
        selected %= static_cast<int>(items.size());
        if (selected < 0) selected += static_cast<int>(items.size());
        if (items[static_cast<usize>(selected)].enabled) return true;
    }
    return false;
}

void MenuList::reset_selection() {
    selected = 0;
    if (!items.empty() && !items.front().enabled) move(1);
}

const MenuItem* MenuList::selected_item() const {
    if (items.empty()) return nullptr;
    const usize i = static_cast<usize>(selected);
    if (i >= items.size()) return nullptr;
    return &items[i];
}

const MenuItem* MenuList::activate() const {
    const MenuItem* it = selected_item();
    if (it == nullptr || !it->enabled) return nullptr;
    return it;
}

// --- Canvas --------------------------------------------------------------------

Label& Canvas::add_label(Label l) {
    labels.push_back(std::move(l));
    return labels.back();
}

ProgressBar& Canvas::add_bar(ProgressBar b) {
    bars.push_back(std::move(b));
    return bars.back();
}

Reticle& Canvas::add_reticle(Reticle r) {
    reticles.push_back(std::move(r));
    return reticles.back();
}

MenuList& Canvas::add_menu(MenuList m) {
    menus.push_back(std::move(m));
    return menus.back();
}

Slider& Canvas::add_slider(Slider s) {
    sliders.push_back(std::move(s));
    return sliders.back();
}

Label* Canvas::find_label(const std::string& id) {
    return const_cast<Label*>(std::as_const(*this).find_label(id));
}

ProgressBar* Canvas::find_bar(const std::string& id) {
    return const_cast<ProgressBar*>(std::as_const(*this).find_bar(id));
}

Reticle* Canvas::find_reticle(const std::string& id) {
    return const_cast<Reticle*>(std::as_const(*this).find_reticle(id));
}

MenuList* Canvas::find_menu(const std::string& id) {
    return const_cast<MenuList*>(std::as_const(*this).find_menu(id));
}

Slider* Canvas::find_slider(const std::string& id) {
    return const_cast<Slider*>(std::as_const(*this).find_slider(id));
}

const Label* Canvas::find_label(const std::string& id) const {
    for (const auto& l : labels) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

const ProgressBar* Canvas::find_bar(const std::string& id) const {
    for (const auto& b : bars) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

const Reticle* Canvas::find_reticle(const std::string& id) const {
    for (const auto& r : reticles) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

const MenuList* Canvas::find_menu(const std::string& id) const {
    for (const auto& m : menus) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const Slider* Canvas::find_slider(const std::string& id) const {
    for (const auto& s : sliders) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

DisplayList Canvas::snapshot(float width, float height) const {
    DisplayList dl;
    if (width <= 0.0f || height <= 0.0f) return dl;

    for (const auto& l : labels) {
        if (!l.visible) continue;
        push_text(dl, l, width, height);
    }

    for (const auto& b : bars) {
        if (!b.visible) continue;
        DlRect back;
        back.rect.x = px(b.rect.x, width);
        back.rect.y = px(b.rect.y, height);
        back.rect.w = px(b.rect.w, width);
        back.rect.h = px(b.rect.h, height);
        back.color = b.background;
        dl.rects.push_back(back);

        const float frac = clamp01(b.fraction);
        if (frac > 0.0f) {
            DlRect fill;
            fill.rect = back.rect;
            fill.rect.w = back.rect.w * frac;
            fill.color = b.fill;
            dl.rects.push_back(fill);
        }
        if (!b.label.empty()) {
            Label lbl;
            lbl.id = b.id + "_label";
            lbl.rect = b.rect;
            lbl.text = b.label;
            lbl.size = b.label_size;
            lbl.align = TextAlign::Center;
            lbl.visible = true;
            push_text(dl, lbl, width, height);
        }
    }

    for (const auto& r : reticles) {
        if (!r.visible) continue;
        const float cx = px(r.x, width);
        const float cy = px(r.y, height);
        const float rad = px(r.radius, std::min(width, height));
        const float gap = px(r.gap, std::min(width, height));
        // Four arms: up, down, left, right — each starts at the gap.
        DlLine arm;
        arm.width = r.thickness;
        arm.color = r.color;
        arm.x1 = cx; arm.y1 = cy - gap; arm.x2 = cx; arm.y2 = cy - rad;
        dl.lines.push_back(arm);
        arm.y1 = cy + gap; arm.y2 = cy + rad;
        dl.lines.push_back(arm);
        arm.x1 = cx - gap; arm.x2 = cx - rad; arm.y1 = cy; arm.y2 = cy;
        dl.lines.push_back(arm);
        arm.x1 = cx + gap; arm.x2 = cx + rad;
        dl.lines.push_back(arm);
        // Center dot (2x2 px).
        DlRect dot;
        dot.rect.x = cx - 1.0f;
        dot.rect.y = cy - 1.0f;
        dot.rect.w = 2.0f;
        dot.rect.h = 2.0f;
        dot.color = r.color;
        dl.rects.push_back(dot);
    }

    for (const auto& m : menus) {
        if (!m.title.empty()) {
            Label title;
            title.id = m.id + "_title";
            title.rect = m.rect;
            title.text = m.title;
            title.size = m.title_size;
            title.align = TextAlign::Center;
            push_text(dl, title, width, height);
        }
        // Items flow downward from the top of the rect (plus one title line).
        float y = m.rect.y + (m.title.empty() ? 0.0f : m.title_size / height);
        const float line_h = m.item_size / height;
        for (usize i = 0; i < m.items.size(); ++i) {
            const MenuItem& item = m.items[i];
            Label lbl;
            lbl.id = m.id + "_item_" + item.id;
            lbl.rect.x = m.rect.x;
            lbl.rect.y = y;
            lbl.rect.w = m.rect.w;
            lbl.rect.h = line_h;
            lbl.size = m.item_size;
            lbl.text = (static_cast<int>(i) == m.selected ? std::string("> ") + item.label
                                                         : std::string("  ") + item.label);
            lbl.color = (static_cast<int>(i) == m.selected) ? m.selected_color : m.color;
            if (!item.enabled) lbl.color.a *= 0.35f;
            lbl.visible = true;
            push_text(dl, lbl, width, height);
            y += line_h;
        }
    }

    for (const auto& s : sliders) {
        if (!s.visible) continue;
        // "Label: NN%" line above a thin track + knob.
        Label lbl;
        lbl.id = s.id + "_label";
        lbl.rect.x = s.rect.x;
        lbl.rect.y = s.rect.y;
        lbl.rect.w = s.rect.w;
        lbl.rect.h = s.rect.h;
        lbl.size = 22.0f;
        char pct[8];
        std::snprintf(pct, sizeof(pct), "%d%%",
                      static_cast<int>(clamp01(s.value) * 100.0f + 0.5f));
        lbl.text = s.label + ": " + pct;
        push_text(dl, lbl, width, height);

        const float track_y = px(s.rect.y, height) + 22.0f + 8.0f;
        DlRect track;
        track.rect.x = px(s.rect.x, width);
        track.rect.y = track_y;
        track.rect.w = px(s.rect.w, width);
        track.rect.h = 6.0f;
        track.color = UiColor{0.3f, 0.3f, 0.3f, 0.8f};
        dl.rects.push_back(track);

        DlRect knob;
        knob.rect.w = 12.0f;
        knob.rect.h = 18.0f;
        knob.rect.x = track.rect.x + (track.rect.w - knob.rect.w) * clamp01(s.value);
        knob.rect.y = track_y - 6.0f;
        knob.color = UiColor{1.0f, 0.8f, 0.2f, 1.0f};
        dl.rects.push_back(knob);
    }

    return dl;
}

} // namespace nf::ui
