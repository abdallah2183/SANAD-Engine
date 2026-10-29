// NF/AI/BehaviorTree.cpp — reactive behavior tree nodes.

#include <NF/AI/BehaviorTree.hpp>

namespace nf::ai {

void Blackboard::set_number(const std::string& key, double value) {
    m_numbers[key] = value;
    notify_observers(key);
}

double Blackboard::get_number(const std::string& key, double fallback) const {
    const auto it = m_numbers.find(key);
    if (it != m_numbers.end()) {
        return it->second;
    }
    return m_parent ? m_parent->get_number(key, fallback) : fallback;
}

bool Blackboard::has_number(const std::string& key) const {
    if (m_numbers.find(key) != m_numbers.end()) {
        return true;
    }
    return m_parent ? m_parent->has_number(key) : false;
}

void Blackboard::set_flag(const std::string& key, bool value) {
    m_flags[key] = value;
    notify_observers(key);
}

bool Blackboard::get_flag(const std::string& key, bool fallback) const {
    const auto it = m_flags.find(key);
    if (it != m_flags.end()) {
        return it->second;
    }
    return m_parent ? m_parent->get_flag(key, fallback) : fallback;
}

bool Blackboard::has_flag(const std::string& key) const {
    if (m_flags.find(key) != m_flags.end()) {
        return true;
    }
    return m_parent ? m_parent->has_flag(key) : false;
}

void Blackboard::set_text(const std::string& key, std::string value) {
    m_texts[key] = std::move(value);
    notify_observers(key);
}

std::string Blackboard::get_text(const std::string& key, const std::string& fallback) const {
    const auto it = m_texts.find(key);
    if (it != m_texts.end()) {
        return it->second;
    }
    return m_parent ? m_parent->get_text(key, fallback) : fallback;
}

bool Blackboard::has_text(const std::string& key) const {
    if (m_texts.find(key) != m_texts.end()) {
        return true;
    }
    return m_parent ? m_parent->has_text(key) : false;
}

void Blackboard::set_vector(const std::string& key, const Vec3& value) {
    m_vectors[key] = value;
    notify_observers(key);
}

Vec3 Blackboard::get_vector(const std::string& key, const Vec3& fallback) const {
    const auto it = m_vectors.find(key);
    if (it != m_vectors.end()) {
        return it->second;
    }
    return m_parent ? m_parent->get_vector(key, fallback) : fallback;
}

bool Blackboard::has_vector(const std::string& key) const {
    if (m_vectors.find(key) != m_vectors.end()) {
        return true;
    }
    return m_parent ? m_parent->has_vector(key) : false;
}

void Blackboard::set_entity(const std::string& key, u32 entity_id) {
    m_entities[key] = entity_id;
    notify_observers(key);
}

u32 Blackboard::get_entity(const std::string& key, u32 fallback) const {
    const auto it = m_entities.find(key);
    if (it != m_entities.end()) {
        return it->second;
    }
    return m_parent ? m_parent->get_entity(key, fallback) : fallback;
}

bool Blackboard::has_entity(const std::string& key) const {
    if (m_entities.find(key) != m_entities.end()) {
        return true;
    }
    return m_parent ? m_parent->has_entity(key) : false;
}

bool Blackboard::has(const std::string& key) const {
    if (has_number(key) || has_flag(key) || has_text(key) || has_vector(key) || has_entity(key)) {
        return true;
    }
    return false;
}

void Blackboard::unset(const std::string& key) {
    bool removed = false;
    removed |= (m_numbers.erase(key) > 0);
    removed |= (m_flags.erase(key) > 0);
    removed |= (m_texts.erase(key) > 0);
    removed |= (m_vectors.erase(key) > 0);
    removed |= (m_entities.erase(key) > 0);
    if (removed) {
        notify_observers(key);
    }
}

void Blackboard::clear() {
    m_numbers.clear();
    m_flags.clear();
    m_texts.clear();
    m_vectors.clear();
    m_entities.clear();
}

u32 Blackboard::register_observer(const std::string& key, ObserverCallback callback) {
    if (!callback) return 0;
    const u32 id = m_next_observer_id++;
    m_observers.push_back({id, key, std::move(callback)});
    return id;
}

void Blackboard::unregister_observer(u32 handle) {
    m_observers.erase(
        std::remove_if(m_observers.begin(), m_observers.end(), [handle](const ObserverEntry& e) {
            return e.id == handle;
        }),
        m_observers.end()
    );
}

void Blackboard::notify_observers(const std::string& key) {
    for (const auto& obs : m_observers) {
        if (obs.key == key || obs.key.empty()) {
            obs.callback(key);
        }
    }
}

BTStatus BTSequence::tick(Blackboard& bb, float dt) {
    if (m_children.empty()) return BTStatus::Success; // vacuous truth
    for (auto& child : m_children) {
        const BTStatus s = child->tick(bb, dt);
        if (s != BTStatus::Success) return s; // Failure or Running
    }
    return BTStatus::Success;
}

void BTSequence::reset() {
    for (auto& child : m_children) child->reset();
}

BTStatus BTSelector::tick(Blackboard& bb, float dt) {
    if (m_children.empty()) return BTStatus::Failure; // nothing to fall back to
    for (auto& child : m_children) {
        const BTStatus s = child->tick(bb, dt);
        if (s != BTStatus::Failure) return s; // Success or Running
    }
    return BTStatus::Failure;
}

void BTSelector::reset() {
    for (auto& child : m_children) child->reset();
}

BTStatus BTInverter::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    if (s == BTStatus::Success) return BTStatus::Failure;
    if (s == BTStatus::Failure) return BTStatus::Success;
    return BTStatus::Running;
}

void BTInverter::reset() {
    if (m_child) m_child->reset();
}

BTStatus BTRepeat::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    if (m_times <= 0) {
        // Forever: tick once per frame, never complete (Running survives).
        m_child->tick(bb, dt);
        return BTStatus::Running;
    }
    while (m_done < m_times) {
        const BTStatus s = m_child->tick(bb, dt);
        if (s == BTStatus::Running) return BTStatus::Running;
        if (s == BTStatus::Failure) {
            m_done = 0; // failure restarts the count, still working
            return BTStatus::Running;
        }
        ++m_done;
        if (m_done >= m_times) return BTStatus::Success;
        // Success but more iterations remain: loop again this same tick so a
        // Repeat of instant children finishes in one frame, not N frames.
    }
    return BTStatus::Success;
}

void BTRepeat::reset() {
    m_done = 0;
    if (m_child) m_child->reset();
}

BTStatus BTWait::tick(Blackboard&, float dt) {
    m_elapsed += dt > 0.0f ? dt : 0.0f;
    return m_elapsed >= m_seconds ? BTStatus::Success : BTStatus::Running;
}

void BTWait::reset() {
    m_elapsed = 0.0f;
}

BTStatus BehaviorTree::tick(Blackboard& bb, float dt) {
    if (!m_root) return BTStatus::Failure;
    return m_root->tick(bb, dt);
}

void BehaviorTree::reset() {
    if (m_root) m_root->reset();
}

BTStatus BTParallel::tick(Blackboard& bb, float dt) {
    if (m_children.empty()) return BTStatus::Success;

    size_t success_count = 0;
    size_t failure_count = 0;

    for (auto& child : m_children) {
        const BTStatus s = child->tick(bb, dt);
        if (s == BTStatus::Success) {
            success_count++;
        } else if (s == BTStatus::Failure) {
            failure_count++;
        }
    }

    if (m_policy == BTParallelPolicy::RequireAll) {
        if (failure_count > 0) return BTStatus::Failure;
        if (success_count == m_children.size()) return BTStatus::Success;
        return BTStatus::Running;
    } else {
        if (success_count > 0) return BTStatus::Success;
        if (failure_count == m_children.size()) return BTStatus::Failure;
        return BTStatus::Running;
    }
}

void BTParallel::reset() {
    for (auto& child : m_children) child->reset();
}

BTStatus BTCooldown::tick(Blackboard& bb, float dt) {
    m_time_since_last_success += (dt > 0.0f) ? dt : 0.0f;
    if (m_time_since_last_success < m_cooldown) {
        return BTStatus::Failure;
    }
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    if (s == BTStatus::Success) {
        m_time_since_last_success = 0.0f;
    }
    return s;
}

void BTCooldown::reset() {
    m_time_since_last_success = 1e9f;
    if (m_child) m_child->reset();
}

BTStatus BTRetry::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    if (s == BTStatus::Success) {
        m_current_retries = 0;
        return BTStatus::Success;
    }
    if (s == BTStatus::Running) {
        return BTStatus::Running;
    }

    m_current_retries++;
    if (m_current_retries >= m_max_retries) {
        m_current_retries = 0;
        return BTStatus::Failure;
    }
    m_child->reset();
    return BTStatus::Running;
}

void BTRetry::reset() {
    m_current_retries = 0;
    if (m_child) m_child->reset();
}

BTStatus BTTimeLimit::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    m_elapsed += (dt > 0.0f) ? dt : 0.0f;
    if (m_elapsed > m_max_seconds) {
        m_child->reset();
        m_elapsed = 0.0f;
        return BTStatus::Failure;
    }
    const BTStatus s = m_child->tick(bb, dt);
    if (s != BTStatus::Running) {
        m_elapsed = 0.0f;
    }
    return s;
}

void BTTimeLimit::reset() {
    m_elapsed = 0.0f;
    if (m_child) m_child->reset();
}

BTStatus BTForceSuccess::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Success;
    const BTStatus s = m_child->tick(bb, dt);
    return (s == BTStatus::Running) ? BTStatus::Running : BTStatus::Success;
}

void BTForceSuccess::reset() {
    if (m_child) m_child->reset();
}

BTStatus BTForceFailure::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    return (s == BTStatus::Running) ? BTStatus::Running : BTStatus::Failure;
}

void BTForceFailure::reset() {
    if (m_child) m_child->reset();
}

void BTService::update(Blackboard& bb, float dt) {
    m_elapsed += (dt > 0.0f) ? dt : 0.0f;
    if (m_elapsed >= m_interval) {
        tick_service(bb, dt);
        m_elapsed = 0.0f;
    }
}

void BTService::reset() {
    m_elapsed = 0.0f;
}

BTStatus BTServiceNode::tick(Blackboard& bb, float dt) {
    if (!m_child) {
        return BTStatus::Failure;
    }

    if (!m_was_active) {
        for (auto& s : m_services) {
            s->on_service_enter(bb);
        }
        m_was_active = true;
    }

    for (auto& s : m_services) {
        s->update(bb, dt);
    }

    const BTStatus status = m_child->tick(bb, dt);
    if (status != BTStatus::Running) {
        for (auto& s : m_services) {
            s->on_service_exit(bb);
        }
        m_was_active = false;
    }

    return status;
}

void BTServiceNode::reset() {
    for (auto& s : m_services) {
        s->reset();
    }
    m_was_active = false;
    if (m_child) {
        m_child->reset();
    }
}

bool BBCondition::evaluate(const Blackboard& bb) const {
    switch (op) {
        case BBConditionOp::IsSet:
            return bb.has(key);
        case BBConditionOp::IsNotSet:
            return !bb.has(key);
        case BBConditionOp::BoolEquals:
            return bb.has_flag(key) && (bb.get_flag(key) == bool_val);
        case BBConditionOp::NumberEquals:
            return bb.has_number(key) && (std::fabs(bb.get_number(key) - number_val) < 1e-5);
        case BBConditionOp::NumberLessThan:
            return bb.has_number(key) && (bb.get_number(key) < number_val);
        case BBConditionOp::NumberGreaterThan:
            return bb.has_number(key) && (bb.get_number(key) > number_val);
        case BBConditionOp::TextEquals:
            return bb.has_text(key) && (bb.get_text(key) == text_val);
        case BBConditionOp::DistanceLess: {
            if (!bb.has_vector(key)) return false;
            const Vec3 pos = bb.get_vector(key);
            return (pos - vector_val).length() <= distance_threshold;
        }
        case BBConditionOp::DistanceGreater: {
            if (!bb.has_vector(key)) return false;
            const Vec3 pos = bb.get_vector(key);
            return (pos - vector_val).length() >= distance_threshold;
        }
    }
    return false;
}

BTStatus BTBlackboardDecorator::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    if (!m_condition.evaluate(bb)) {
        return BTStatus::Failure;
    }
    return m_child->tick(bb, dt);
}

void BTBlackboardDecorator::reset() {
    if (m_child) m_child->reset();
}

BTBuilder::BTBuilder() = default;

void BTBuilder::add_child(std::unique_ptr<BTNode> child) {
    if (!m_stack.empty()) {
        m_stack.back().children.push_back(std::move(child));
    } else {
        m_root = std::move(child);
    }
}

BTBuilder& BTBuilder::sequence() {
    m_stack.push_back({FrameType::Sequence, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::selector() {
    m_stack.push_back({FrameType::Selector, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::parallel(BTParallelPolicy policy) {
    m_stack.push_back({(policy == BTParallelPolicy::RequireAll) ? FrameType::ParallelAll : FrameType::ParallelOne, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::inverter() {
    m_stack.push_back({FrameType::Inverter, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::repeat(int times) {
    m_stack.push_back({FrameType::Repeat, 0.0f, times, {}});
    return *this;
}

BTBuilder& BTBuilder::cooldown(float seconds) {
    m_stack.push_back({FrameType::Cooldown, seconds, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::retry(int max_retries) {
    m_stack.push_back({FrameType::Retry, 0.0f, max_retries, {}});
    return *this;
}

BTBuilder& BTBuilder::time_limit(float max_seconds) {
    m_stack.push_back({FrameType::TimeLimit, max_seconds, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::force_success() {
    m_stack.push_back({FrameType::ForceSuccess, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::force_failure() {
    m_stack.push_back({FrameType::ForceFailure, 0.0f, 0});
    return *this;
}

BTBuilder& BTBuilder::service(float interval, std::function<void(Blackboard&, float)> fn) {
    StackFrame frame;
    frame.type = FrameType::ServiceNode;
    frame.param_f = interval;
    frame.service_fn = std::move(fn);
    m_stack.push_back(std::move(frame));
    return *this;
}

BTBuilder& BTBuilder::blackboard_check(BBCondition condition) {
    StackFrame frame;
    frame.type = FrameType::BlackboardDecorator;
    frame.bb_condition = std::move(condition);
    m_stack.push_back(std::move(frame));
    return *this;
}

BTBuilder& BTBuilder::wait(float seconds) {
    add_child(std::make_unique<BTWait>(seconds));
    return *this;
}

BTBuilder& BTBuilder::action(std::function<BTStatus(Blackboard&, float)> fn) {
    add_child(std::make_unique<BTAction>(std::move(fn)));
    return *this;
}

BTBuilder& BTBuilder::condition(std::function<bool(const Blackboard&)> fn) {
    add_child(std::make_unique<BTCondition>(std::move(fn)));
    return *this;
}

BTBuilder& BTBuilder::end() {
    if (m_stack.empty()) return *this;

    StackFrame frame = std::move(m_stack.back());
    m_stack.pop_back();

    std::unique_ptr<BTNode> node;
    switch (frame.type) {
        case FrameType::Sequence: {
            auto seq = std::make_unique<BTSequence>();
            for (auto& c : frame.children) seq->add(std::move(c));
            node = std::move(seq);
            break;
        }
        case FrameType::Selector: {
            auto sel = std::make_unique<BTSelector>();
            for (auto& c : frame.children) sel->add(std::move(c));
            node = std::move(sel);
            break;
        }
        case FrameType::ParallelAll: {
            auto par = std::make_unique<BTParallel>(BTParallelPolicy::RequireAll);
            for (auto& c : frame.children) par->add(std::move(c));
            node = std::move(par);
            break;
        }
        case FrameType::ParallelOne: {
            auto par = std::make_unique<BTParallel>(BTParallelPolicy::RequireOne);
            for (auto& c : frame.children) par->add(std::move(c));
            node = std::move(par);
            break;
        }
        case FrameType::Inverter: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTInverter>(std::move(child));
            break;
        }
        case FrameType::Repeat: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTRepeat>(std::move(child), frame.param_i);
            break;
        }
        case FrameType::Cooldown: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTCooldown>(std::move(child), frame.param_f);
            break;
        }
        case FrameType::Retry: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTRetry>(std::move(child), frame.param_i);
            break;
        }
        case FrameType::TimeLimit: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTTimeLimit>(std::move(child), frame.param_f);
            break;
        }
        case FrameType::ForceSuccess: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTForceSuccess>(std::move(child));
            break;
        }
        case FrameType::ForceFailure: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTForceFailure>(std::move(child));
            break;
        }
        case FrameType::ServiceNode: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            auto s_node = std::make_unique<BTServiceNode>(std::move(child));
            if (frame.service_fn) {
                s_node->add_service(std::make_unique<BTLambdaService>(frame.param_f, std::move(frame.service_fn)));
            }
            node = std::move(s_node);
            break;
        }
        case FrameType::BlackboardDecorator: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTBlackboardDecorator>(std::move(child), std::move(frame.bb_condition));
            break;
        }
    }

    if (node) {
        add_child(std::move(node));
    }
    return *this;
}

std::unique_ptr<BehaviorTree> BTBuilder::build() {
    while (!m_stack.empty()) {
        end();
    }
    return std::make_unique<BehaviorTree>(std::move(m_root));
}

} // namespace nf::ai
