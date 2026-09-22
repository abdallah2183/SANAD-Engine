// NF/Input/InputMapper.cpp

#include <NF/Input/InputMapper.hpp>
#include <NF/Input/InputNames.hpp>

#include <NF/Core/Assert.hpp>
#include <NF/Core/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <utility>

namespace nf::input {

namespace {

/// Splits a line into whitespace-separated tokens, reusing the caller's storage.
void tokenize(std::string_view line, DynamicArray<std::string_view>& out) {
    out.clear();
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i])) != 0) {
            ++i;
        }
        if (i >= line.size()) break;
        const usize start = i;
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i])) == 0) {
            ++i;
        }
        out.push_back(line.substr(start, i - start));
    }
}

bool parse_i32(std::string_view token, i32& out) {
    const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
    return res.ec == std::errc();
}

bool parse_f32(std::string_view token, f32& out) {
    const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
    return res.ec == std::errc();
}

} // namespace

// ---------------------------------------------------------------------------
// Contexts
// ---------------------------------------------------------------------------

InputContext& InputMapper::create_context(std::string name, i32 priority,
                                          bool blocking) {
    if (InputContext* existing = find_context(name)) {
        NF_LOG_WARN(LogCategory::Platform,
                    "InputMapper: duplicate context '{}'", name);
        return *existing;
    }
    auto ctx = std::make_unique<InputContext>(std::move(name), priority);
    ctx->set_blocking(blocking);
    InputContext& ref = *ctx;
    m_contexts.push_back(std::move(ctx));
    mark_dirty();
    return ref;
}

InputContext* InputMapper::find_context(std::string_view name) {
    for (const std::unique_ptr<InputContext>& ctx : m_contexts) {
        if (ctx->name() == name) return ctx.get();
    }
    return nullptr;
}

const InputContext* InputMapper::find_context(std::string_view name) const {
    for (const std::unique_ptr<InputContext>& ctx : m_contexts) {
        if (ctx->name() == name) return ctx.get();
    }
    return nullptr;
}

void InputMapper::set_context_active(std::string_view name, bool active) {
    if (InputContext* ctx = find_context(name)) {
        ctx->set_active(active);
    } else {
        NF_LOG_WARN(LogCategory::Platform,
                    "InputMapper: unknown context '{}'", name);
    }
}

void InputMapper::set_all_active(bool active) {
    for (const std::unique_ptr<InputContext>& ctx : m_contexts) {
        ctx->set_active(active);
    }
}

void InputMapper::mark_dirty() {
    m_dirty = true;
}

void InputMapper::refresh_cache() {
    m_dirty = false;

    const usize n = m_contexts.size();
    m_context_order.clear();
    for (usize i = 0; i < n; ++i) {
        m_context_order.push_back(i);
    }
    m_context_suppressed.clear();
    m_context_suppressed.resize(n, false);

    // Stable sort by priority descending: contexts with equal priority keep
    // insertion order, so evaluation order — and therefore every action value —
    // is reproducible run to run (design doc §114).
    std::stable_sort(m_context_order.begin(), m_context_order.end(),
                     [this](usize a, usize b) {
                         return m_contexts[a]->priority() > m_contexts[b]->priority();
                     });

    // Pre-size the value maps so update() never rehashes mid-frame.
    usize total_actions = 0;
    for (const std::unique_ptr<InputContext>& ctx : m_contexts) {
        total_actions += ctx->action_count();
    }
    m_values_a.reserve(total_actions);
    m_values_b.reserve(total_actions);

    compute_suppression();
}

void InputMapper::compute_suppression() {
    // Visited in priority order: once an active blocking context is seen, every
    // following (lower-priority) context is suppressed for this frame.
    bool blocking_active = false;
    for (const usize ci : m_context_order) {
        const InputContext& ctx = *m_contexts[ci];
        m_context_suppressed[ci] = blocking_active;
        if (ctx.active() && ctx.blocking()) {
            blocking_active = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

void InputMapper::update(const InputState& state) {
    if (m_dirty) refresh_cache();
    compute_suppression();

    for (const usize ci : m_context_order) {
        const InputContext& ctx = *m_contexts[ci];
        const bool live = ctx.active() && !m_context_suppressed[ci];
        for (const InputAction& action : ctx.actions()) {
            ActionValue value{};
            if (live) {
                switch (action.type()) {
                case ActionType::Bool:
                    value.x = action.evaluate_bool(state, m_pad);
                    break;
                case ActionType::Axis1D:
                    value.x = action.evaluate_axis(state, m_pad);
                    break;
                case ActionType::Axis2D: {
                    const Vec2 v = action.evaluate_vector(state, m_pad);
                    value.x = v.x;
                    value.y = v.y;
                    break;
                }
                }
            }
            m_current->put(action.name(), value);
        }
    }

    fire_callbacks();
}

void InputMapper::set_action_callback(std::string_view action,
                                      ActionPhase phase, ActionCallback callback) {
    const std::string key(action);
    ActionCallbacks* slots = m_callbacks.get(key);
    if (slots == nullptr) {
        m_callbacks.put(key, ActionCallbacks{});
        slots = m_callbacks.get(key);
    }
    switch (phase) {
    case ActionPhase::Pressed:
        slots->on_pressed = std::move(callback);
        break;
    case ActionPhase::Held:
        slots->on_held = std::move(callback);
        break;
    case ActionPhase::Released:
        slots->on_released = std::move(callback);
        break;
    }
}

void InputMapper::fire_callbacks() {
    if (m_callbacks.empty()) return;

    // Evaluation order (context priority, then insertion) — identical to the
    // value pass, so callback sequencing is reproducible (design doc §114).
    // Every action reached here was just put into m_current, so its value
    // pointer is live; the previous frame may not exist yet (first update).
    for (const usize ci : m_context_order) {
        const InputContext& ctx = *m_contexts[ci];
        for (const InputAction& action : ctx.actions()) {
            const ActionCallbacks* slots = m_callbacks.get(action.name());
            if (slots == nullptr) continue;
            if (!slots->on_pressed && !slots->on_held && !slots->on_released) continue;

            // Edge semantics mirror just_pressed / just_released exactly:
            // a missing previous frame counts as "was not pressed".
            const ActionValue* cur = m_current->get(action.name());
            const ActionValue* prev = m_previous->get(action.name());
            const bool pressed_now = cur != nullptr && cur->pressed();
            const bool pressed_before = prev != nullptr && prev->pressed();

            if (pressed_now && !pressed_before) {
                if (slots->on_pressed) slots->on_pressed(*cur);
            } else if (pressed_now && pressed_before) {
                if (slots->on_held) slots->on_held(*cur);
            } else if (!pressed_now && pressed_before) {
                if (slots->on_released) slots->on_released(*cur);
            }
        }
    }
}

void InputMapper::end_frame() {
    // O(1) swap: this frame's values become next frame's reference. The maps
    // keep their key sets, so no allocation happens here either.
    std::swap(m_current, m_previous);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const ActionValue* InputMapper::current_value(std::string_view action) const {
    return m_current->get(std::string(action));
}

const ActionValue* InputMapper::previous_value(std::string_view action) const {
    return m_previous->get(std::string(action));
}

bool InputMapper::is_pressed(std::string_view action) const {
    const ActionValue* v = current_value(action);
    return v != nullptr && v->pressed();
}

bool InputMapper::just_pressed(std::string_view action) const {
    const ActionValue* cur = current_value(action);
    if (cur == nullptr || !cur->pressed()) return false;
    const ActionValue* prev = previous_value(action);
    return prev == nullptr || !prev->pressed();
}

bool InputMapper::just_released(std::string_view action) const {
    const ActionValue* cur = current_value(action);
    if (cur == nullptr || cur->pressed()) return false;
    const ActionValue* prev = previous_value(action);
    return prev != nullptr && prev->pressed();
}

f32 InputMapper::get_axis(std::string_view action) const {
    const ActionValue* v = current_value(action);
    return v != nullptr ? v->x : 0.0f;
}

Vec2 InputMapper::get_vector(std::string_view action) const {
    const ActionValue* v = current_value(action);
    return v != nullptr ? Vec2{v->x, v->y} : Vec2{0.0f, 0.0f};
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string InputMapper::to_string() const {
    std::string out;
    out += "# NOVAForge input bindings\n";
    out += "v ";
    out += std::to_string(BindingsVersion);
    out += "\n";

    for (const std::unique_ptr<InputContext>& ctx : m_contexts) {
        out += "c ";
        out += ctx->name();
        out += " ";
        out += std::to_string(ctx->priority());
        out += " ";
        out += ctx->blocking() ? "1" : "0";
        out += "\n";

        for (const InputAction& action : ctx->actions()) {
            out += "a ";
            out += ctx->name();
            out += " ";
            out += action.name();
            out += " ";
            out += std::string(::nf::input::to_string(action.type()));
            out += "\n";

            for (const InputBinding& b : action.bindings()) {
                out += "b ";
                out += ctx->name();
                out += " ";
                out += action.name();
                out += " ";
                out += std::string(::nf::input::to_string(b.source));
                out += " ";
                switch (b.source) {
                case BindingSource::Key:
                    out += std::string(::nf::input::to_string(b.key));
                    break;
                case BindingSource::Mouse:
                    out += std::string(::nf::input::to_string(b.mouse));
                    break;
                case BindingSource::GamepadButton:
                    out += std::string(::nf::input::to_string(b.pad_button));
                    break;
                case BindingSource::GamepadAxis:
                    out += std::string(::nf::input::to_string(b.pad_axis));
                    break;
                }
                out += " ";
                out += std::string(::nf::input::to_string(b.component));
                out += " ";
                out += std::to_string(b.scale);
                out += "\n";
            }
        }
    }
    return out;
}

bool InputMapper::load_string(std::string_view text) {
    m_contexts.clear();
    m_context_order.clear();
    m_context_suppressed.clear();
    m_values_a.clear();
    m_values_b.clear();
    m_callbacks.clear(); // actions are rebuilt; stale callbacks must not fire
    mark_dirty();

    DynamicArray<std::string_view> tokens;
    bool ok = true;
    usize pos = 0;
    while (pos < text.size()) {
        usize eol = pos;
        while (eol < text.size() && text[eol] != '\n' && text[eol] != '\r') {
            ++eol;
        }
        const std::string_view line = text.substr(pos, eol - pos);
        pos = (eol < text.size()) ? eol + 1 : eol;

        tokenize(line, tokens);
        if (tokens.empty()) continue;
        if (tokens.front().starts_with('#')) continue;

        const auto count = [&](usize n) { return tokens.size() >= n; };
        const auto tok = [&](usize i) -> std::string_view {
            return i < tokens.size() ? tokens[i] : std::string_view{};
        };

        const std::string_view cmd = tokens.front();
        if (cmd == "v") {
            // Version line: refuse a dump from an incompatible schema (§242).
            if (!count(2)) { ok = false; continue; }
            i32 version = 0;
            if (!parse_i32(tok(1), version) ||
                static_cast<u32>(version) != BindingsVersion) {
                NF_LOG_ERROR(LogCategory::Platform,
                             "InputMapper: unsupported bindings version");
                return false;
            }
        } else if (cmd == "c") {
            if (!count(4)) { ok = false; continue; }
            i32 priority = 0;
            if (!parse_i32(tok(2), priority)) { ok = false; continue; }
            create_context(std::string(tok(1)), priority, tok(3) == "1");
        } else if (cmd == "a") {
            if (!count(4)) { ok = false; continue; }
            InputContext* ctx = find_context(tok(1));
            if (ctx == nullptr) { ok = false; continue; }
            ActionType type = ActionType::Bool;
            if (!parse_action_type(tok(3), type)) { ok = false; continue; }
            ctx->add_action(InputAction(std::string(tok(2)), type));
        } else if (cmd == "b") {
            if (!count(7)) { ok = false; continue; }
            InputContext* ctx = find_context(tok(1));
            if (ctx == nullptr) { ok = false; continue; }
            InputAction* action = ctx->find_action(tok(2));
            if (action == nullptr) { ok = false; continue; }

            BindingSource source = BindingSource::Key;
            if (!parse_binding_source(tok(3), source)) { ok = false; continue; }
            AxisComponent component = AxisComponent::None;
            if (!parse_axis_component(tok(5), component)) { ok = false; continue; }
            f32 scale = 1.0f;
            if (!parse_f32(tok(6), scale)) { ok = false; continue; }

            switch (source) {
            case BindingSource::Key: {
                KeyCode key = KeyCode::Unknown;
                if (!parse_key(tok(4), key) || key == KeyCode::Unknown ||
                    key == KeyCode::Count) {
                    ok = false; break;
                }
                action->add_key(key, component, scale);
                break;
            }
            case BindingSource::Mouse: {
                MouseButton button = MouseButton::Count;
                if (!parse_mouse_button(tok(4), button) ||
                    button == MouseButton::Count) {
                    ok = false; break;
                }
                action->add_mouse(button, component, scale);
                break;
            }
            case BindingSource::GamepadButton: {
                GamepadButton button = GamepadButton::Count;
                if (!parse_gamepad_button(tok(4), button) ||
                    button == GamepadButton::Count) {
                    ok = false; break;
                }
                action->add_gamepad_button(button, component, scale);
                break;
            }
            case BindingSource::GamepadAxis: {
                GamepadAxis axis = GamepadAxis::Count;
                if (!parse_gamepad_axis(tok(4), axis) ||
                    axis == GamepadAxis::Count) {
                    ok = false; break;
                }
                action->add_gamepad_axis(axis, component, scale);
                break;
            }
            }
        } else {
            NF_LOG_WARN(LogCategory::Platform,
                        "InputMapper: unknown binding command '{}'", cmd);
            ok = false;
        }
    }
    return ok;
}

} // namespace nf::input
