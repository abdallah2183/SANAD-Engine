#pragma once

#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace nf::scripting {
class LuaVM;

// Runtime-owned commands and bounded, in-memory session data. Lua only queues
// commands; the host replaces scenes after all script calls have returned.
class SceneFlow {
public:
    using Value = std::variant<bool, double, std::string>;
    bool request_scene(std::string_view path, std::string& error);
    bool request_restart(std::string& error);
    bool request_quit(std::string& error);
    [[nodiscard]] bool pending() const { return !m_pending.empty(); }
    [[nodiscard]] const std::string& pending_path() const { return m_pending; }
    [[nodiscard]] bool quit_requested() const { return m_quit; }
    [[nodiscard]] const std::string& current() const { return m_current; }
    [[nodiscard]] const std::string& status() const { return m_status; }
    [[nodiscard]] const std::string& error() const { return m_error; }
    [[nodiscard]] const std::string& target() const { return m_target; }
    void scene_adopted(const std::string& path);
    void complete(bool success, std::string error = {});
    void reset_session();
    bool set(std::string_view key, Value value, std::string& error);
    bool erase(std::string_view key);
    [[nodiscard]] const Value* get(std::string_view key) const;
    void clear_values() { m_values.clear(); }
    [[nodiscard]] size_t size() const { return m_values.size(); }
    static bool valid_scene_path(std::string_view path);
private:
    std::string m_current, m_pending, m_target, m_error;
    std::string m_status = "idle";
    bool m_quit = false;
    std::map<std::string, Value, std::less<>> m_values;
};

void install_game_flow_bindings(LuaVM& vm, SceneFlow* flow);
} // namespace nf::scripting
