#pragma once

// NF/AI/BehaviorTree.hpp — reactive behavior trees (design doc Section 45).
//
// Sequences try children in order (fail fast), selectors fall back (succeed
// fast), decorators reshape one child, leaves sense and act through
// std::functions against a Blackboard. Nodes may hold tick state (Wait,
// Repeat); BehaviorTree::reset() clears the whole tree.
//
// Semantics (documented, not accidental):
//   - Reactive re-evaluation: a Running child is re-entered from the root
//     every tick (no stored resume pointers), so higher-priority branches
//     preempt. Long actions must be idempotent across ticks.
//   - A tree tick never allocates; node objects own their state.

#include <NF/Core/Types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::ai {

enum class BTStatus : u8 { Success = 0, Failure = 1, Running = 2 };

/// String/number/flag working memory shared by a tree's nodes.
class Blackboard {
public:
    void set_number(const std::string& key, double value) { m_numbers[key] = value; }
    double get_number(const std::string& key, double fallback = 0.0) const {
        const auto it = m_numbers.find(key);
        return it != m_numbers.end() ? it->second : fallback;
    }
    void set_flag(const std::string& key, bool value) { m_flags[key] = value; }
    bool get_flag(const std::string& key, bool fallback = false) const {
        const auto it = m_flags.find(key);
        return it != m_flags.end() ? it->second : fallback;
    }
    void set_text(const std::string& key, std::string value) { m_texts[key] = std::move(value); }
    std::string get_text(const std::string& key, const std::string& fallback = {}) const {
        const auto it = m_texts.find(key);
        return it != m_texts.end() ? it->second : fallback;
    }
    void clear();

private:
    std::unordered_map<std::string, double> m_numbers;
    std::unordered_map<std::string, bool> m_flags;
    std::unordered_map<std::string, std::string> m_texts;
};

class BTNode {
public:
    virtual ~BTNode() = default;
    virtual BTStatus tick(Blackboard& bb, float dt) = 0;
    virtual void reset() {}
};

/// Runs children in order; first Failure/Running wins, else Success.
class BTSequence : public BTNode {
public:
    void add(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::vector<std::unique_ptr<BTNode>> m_children;
};

/// Runs children in order; first Success/Running wins, else Failure.
class BTSelector : public BTNode {
public:
    void add(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::vector<std::unique_ptr<BTNode>> m_children;
};

/// Flips Success <-> Failure; Running passes through.
class BTInverter : public BTNode {
public:
    explicit BTInverter(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
};

/// Runs the child until it succeeds N times (then Success); a Failure
/// restarts the count and keeps Running. Count <= 0 means "forever".
class BTRepeat : public BTNode {
public:
    BTRepeat(std::unique_ptr<BTNode> child, int times) : m_child(std::move(child)), m_times(times) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
    int m_times = 1;
    int m_done = 0;
};

/// Returns Running until `seconds` elapse, then Success.
class BTWait : public BTNode {
public:
    explicit BTWait(float seconds) : m_seconds(seconds) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    float m_seconds = 0.0f;
    float m_elapsed = 0.0f;
};

/// Leaf: runs a function. Return Running for multi-tick actions.
class BTAction : public BTNode {
public:
    explicit BTAction(std::function<BTStatus(Blackboard&, float)> fn) : m_fn(std::move(fn)) {}
    BTStatus tick(Blackboard& bb, float dt) override { return m_fn(bb, dt); }

private:
    std::function<BTStatus(Blackboard&, float)> m_fn;
};

/// Leaf: Success when predicate holds, else Failure.
class BTCondition : public BTNode {
public:
    explicit BTCondition(std::function<bool(const Blackboard&)> fn) : m_fn(std::move(fn)) {}
    BTStatus tick(Blackboard& bb, float) override { return m_fn(bb) ? BTStatus::Success : BTStatus::Failure; }

private:
    std::function<bool(const Blackboard&)> m_fn;
};

class BehaviorTree {
public:
    explicit BehaviorTree(std::unique_ptr<BTNode> root) : m_root(std::move(root)) {}
    BTStatus tick(Blackboard& bb, float dt);
    void reset();

private:
    std::unique_ptr<BTNode> m_root;
};

} // namespace nf::ai
