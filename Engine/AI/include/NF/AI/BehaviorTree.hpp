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

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::ai {

enum class BTStatus : u8 { Success = 0, Failure = 1, Running = 2 };

/// String/number/flag/vector/entity working memory shared by a tree's nodes.
/// Supports hierarchical parent blackboard inheritance and observer change callbacks.
class Blackboard {
public:
    using ObserverCallback = std::function<void(const std::string& key)>;

    void set_number(const std::string& key, double value);
    [[nodiscard]] double get_number(const std::string& key, double fallback = 0.0) const;
    [[nodiscard]] bool has_number(const std::string& key) const;

    void set_flag(const std::string& key, bool value);
    [[nodiscard]] bool get_flag(const std::string& key, bool fallback = false) const;
    [[nodiscard]] bool has_flag(const std::string& key) const;

    void set_text(const std::string& key, std::string value);
    [[nodiscard]] std::string get_text(const std::string& key, const std::string& fallback = {}) const;
    [[nodiscard]] bool has_text(const std::string& key) const;

    void set_vector(const std::string& key, const Vec3& value);
    [[nodiscard]] Vec3 get_vector(const std::string& key, const Vec3& fallback = {}) const;
    [[nodiscard]] bool has_vector(const std::string& key) const;

    void set_entity(const std::string& key, u32 entity_id);
    [[nodiscard]] u32 get_entity(const std::string& key, u32 fallback = 0) const;
    [[nodiscard]] bool has_entity(const std::string& key) const;

    [[nodiscard]] bool has(const std::string& key) const;
    void unset(const std::string& key);
    void clear();

    /// Parent blackboard delegation for squad / collective AI
    void set_parent(const Blackboard* parent) noexcept { m_parent = parent; }
    [[nodiscard]] const Blackboard* parent() const noexcept { return m_parent; }

    /// Observer callbacks on key modifications
    u32 register_observer(const std::string& key, ObserverCallback callback);
    void unregister_observer(u32 handle);

private:
    void notify_observers(const std::string& key);

    const Blackboard* m_parent = nullptr;
    std::unordered_map<std::string, double> m_numbers;
    std::unordered_map<std::string, bool> m_flags;
    std::unordered_map<std::string, std::string> m_texts;
    std::unordered_map<std::string, Vec3> m_vectors;
    std::unordered_map<std::string, u32> m_entities;

    struct ObserverEntry {
        u32 id = 0;
        std::string key;
        ObserverCallback callback;
    };
    std::vector<ObserverEntry> m_observers;
    u32 m_next_observer_id = 1;
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

/// Policy for BTParallel completion.
enum class BTParallelPolicy : u8 {
    RequireAll, // Fails if any child fails, succeeds if all succeed
    RequireOne  // Succeeds if any child succeeds, fails if all fail
};

/// Executes all children concurrently each tick.
class BTParallel : public BTNode {
public:
    explicit BTParallel(BTParallelPolicy policy = BTParallelPolicy::RequireAll) : m_policy(policy) {}
    void add(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    BTParallelPolicy m_policy = BTParallelPolicy::RequireAll;
    std::vector<std::unique_ptr<BTNode>> m_children;
};

/// Prevents execution of child until cooldown seconds have elapsed since last Success.
class BTCooldown : public BTNode {
public:
    BTCooldown(std::unique_ptr<BTNode> child, float cooldown_seconds)
        : m_child(std::move(child)), m_cooldown(cooldown_seconds) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
    float m_cooldown = 0.0f;
    float m_time_since_last_success = 1e9f;
};

/// Retries a failing child node up to max_retries before propagating Failure.
class BTRetry : public BTNode {
public:
    BTRetry(std::unique_ptr<BTNode> child, int max_retries)
        : m_child(std::move(child)), m_max_retries(max_retries) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
    int m_max_retries = 3;
    int m_current_retries = 0;
};

/// Limits child execution to max_seconds; if exceeded, child is reset and returns Failure.
class BTTimeLimit : public BTNode {
public:
    BTTimeLimit(std::unique_ptr<BTNode> child, float max_seconds)
        : m_child(std::move(child)), m_max_seconds(max_seconds) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
    float m_max_seconds = 0.0f;
    float m_elapsed = 0.0f;
};

/// Executes child; returns Success unless child is Running.
class BTForceSuccess : public BTNode {
public:
    explicit BTForceSuccess(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
};

/// Executes child; returns Failure unless child is Running.
class BTForceFailure : public BTNode {
public:
    explicit BTForceFailure(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}
    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
};

/// Periodic background service running while a branch or node is active
class BTService {
public:
    explicit BTService(float interval = 0.2f, float random_deviation = 0.0f)
        : m_interval(std::max(0.01f, interval)), m_deviation(std::max(0.0f, random_deviation)) {}
    virtual ~BTService() = default;

    virtual void tick_service(Blackboard& bb, float dt) = 0;
    virtual void on_service_enter(Blackboard& /*bb*/) {}
    virtual void on_service_exit(Blackboard& /*bb*/) {}

    void update(Blackboard& bb, float dt);
    void reset();

    [[nodiscard]] float interval() const noexcept { return m_interval; }

private:
    float m_interval = 0.2f;
    float m_deviation = 0.0f;
    float m_elapsed = 0.0f;
};

class BTLambdaService : public BTService {
public:
    using ServiceFn = std::function<void(Blackboard&, float)>;
    BTLambdaService(float interval, ServiceFn fn, float deviation = 0.0f)
        : BTService(interval, deviation), m_fn(std::move(fn)) {}

    void tick_service(Blackboard& bb, float dt) override {
        if (m_fn) m_fn(bb, dt);
    }

private:
    ServiceFn m_fn;
};

/// Node that hosts and executes one or more services while its child runs
class BTServiceNode : public BTNode {
public:
    explicit BTServiceNode(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}

    void add_service(std::unique_ptr<BTService> service) {
        if (service) m_services.push_back(std::move(service));
    }

    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
    std::vector<std::unique_ptr<BTService>> m_services;
    bool m_was_active = false;
};

enum class BBConditionOp : u8 {
    IsSet = 0,
    IsNotSet,
    BoolEquals,
    NumberEquals,
    NumberLessThan,
    NumberGreaterThan,
    TextEquals,
    DistanceLess,
    DistanceGreater
};

struct BBCondition {
    std::string key;
    BBConditionOp op = BBConditionOp::IsSet;
    bool bool_val = false;
    double number_val = 0.0;
    std::string text_val;
    Vec3 vector_val{};
    float distance_threshold = 0.0f;

    static BBCondition is_set(std::string k) {
        BBCondition c;
        c.key = std::move(k);
        c.op = BBConditionOp::IsSet;
        return c;
    }
    static BBCondition is_not_set(std::string k) {
        BBCondition c;
        c.key = std::move(k);
        c.op = BBConditionOp::IsNotSet;
        return c;
    }
    static BBCondition bool_equals(std::string k, bool val) {
        BBCondition c;
        c.key = std::move(k);
        c.op = BBConditionOp::BoolEquals;
        c.bool_val = val;
        return c;
    }
    static BBCondition number_compare(std::string k, BBConditionOp op, double val) {
        BBCondition c;
        c.key = std::move(k);
        c.op = op;
        c.number_val = val;
        return c;
    }
    static BBCondition distance_compare(std::string k, BBConditionOp op, const Vec3& target, float dist) {
        BBCondition c;
        c.key = std::move(k);
        c.op = op;
        c.vector_val = target;
        c.distance_threshold = dist;
        return c;
    }

    [[nodiscard]] bool evaluate(const Blackboard& bb) const;
};

/// Executes child only if blackboard condition is met; otherwise returns Failure
class BTBlackboardDecorator : public BTNode {
public:
    BTBlackboardDecorator(std::unique_ptr<BTNode> child, BBCondition condition)
        : m_child(std::move(child)), m_condition(std::move(condition)) {}

    BTStatus tick(Blackboard& bb, float dt) override;
    void reset() override;

private:
    std::unique_ptr<BTNode> m_child;
    BBCondition m_condition;
};

class BehaviorTree {
public:
    explicit BehaviorTree(std::unique_ptr<BTNode> root) : m_root(std::move(root)) {}
    BTStatus tick(Blackboard& bb, float dt);
    void reset();

private:
    std::unique_ptr<BTNode> m_root;
};

/// Fluent builder for declarative behavior tree authoring.
class BTBuilder {
public:
    BTBuilder();
    ~BTBuilder() = default;

    BTBuilder& sequence();
    BTBuilder& selector();
    BTBuilder& parallel(BTParallelPolicy policy = BTParallelPolicy::RequireAll);
    BTBuilder& inverter();
    BTBuilder& repeat(int times);
    BTBuilder& cooldown(float seconds);
    BTBuilder& retry(int max_retries);
    BTBuilder& time_limit(float max_seconds);
    BTBuilder& force_success();
    BTBuilder& force_failure();
    BTBuilder& service(float interval, std::function<void(Blackboard&, float)> fn);
    BTBuilder& blackboard_check(BBCondition condition);
    BTBuilder& wait(float seconds);
    BTBuilder& action(std::function<BTStatus(Blackboard&, float)> fn);
    BTBuilder& condition(std::function<bool(const Blackboard&)> fn);
    BTBuilder& end();

    std::unique_ptr<BehaviorTree> build();

private:
    enum class FrameType {
        Sequence,
        Selector,
        ParallelAll,
        ParallelOne,
        Inverter,
        Repeat,
        Cooldown,
        Retry,
        TimeLimit,
        ForceSuccess,
        ForceFailure,
        ServiceNode,
        BlackboardDecorator
    };

    struct StackFrame {
        FrameType type = FrameType::Sequence;
        float param_f = 0.0f;
        int param_i = 0;
        BBCondition bb_condition{};
        std::function<void(Blackboard&, float)> service_fn{};
        std::vector<std::unique_ptr<BTNode>> children;

        StackFrame() = default;
        StackFrame(FrameType t, float pf = 0.0f, int pi = 0)
            : type(t), param_f(pf), param_i(pi) {}
        StackFrame(FrameType t, float pf, int pi, std::vector<std::unique_ptr<BTNode>> ch)
            : type(t), param_f(pf), param_i(pi), children(std::move(ch)) {}
    };

    std::vector<StackFrame> m_stack;
    std::unique_ptr<BTNode> m_root;

    void add_child(std::unique_ptr<BTNode> child);
};

} // namespace nf::ai
