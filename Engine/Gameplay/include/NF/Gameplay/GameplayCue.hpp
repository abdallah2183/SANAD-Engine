#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::gameplay {

/// Type of gameplay cue event
enum class GameplayCueType : u8 {
    OnActive = 0,    // When an ongoing ability/effect begins
    WhileActive,     // Periodic or frame update for ongoing visual/audio effect
    Executed,        // Instantaneous one-shot burst (e.g. projectile impact, slash, flash)
    Removed          // Cleanup when an ongoing effect or ability terminates
};

/// Context parameters passed along with a gameplay cue
struct GameplayCueParameters {
    Vec3 location{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    f32 raw_magnitude = 0.0f;
    f32 normalized_magnitude = 0.0f;
    u64 instigator_id = 0;
    u64 target_id = 0;
    u64 effect_causer_id = 0;
    std::string physical_material;
};

/// Callback invoked when a gameplay cue is dispatched
using GameplayCueHandler = std::function<void(GameplayCueType type, const GameplayCueParameters& params)>;

/// Record of a dispatched cue (used for deterministic replays, testing, and telemetry)
struct RecordedCueEvent {
    GameplayCueType type = GameplayCueType::Executed;
    std::string tag;
    GameplayCueParameters params;
};

/// Gameplay Cue Manager
/// Dispatches visual, auditory, and haptic feedback based on hierarchical gameplay tags,
/// decoupling presentation logic from core combat and ability logic.
class GameplayCueManager {
public:
    GameplayCueManager() = default;
    ~GameplayCueManager() = default;

    /// Register a handler callback for a specific gameplay cue tag
    void register_handler(const std::string& cue_tag, GameplayCueHandler handler);

    /// Unregister all handler callbacks for a specific tag
    void unregister_handler(const std::string& cue_tag);

    /// Clear all registered handlers
    void clear_handlers();

    [[nodiscard]] size_t handler_count() const noexcept;
    [[nodiscard]] bool has_handler_for_tag(const std::string& cue_tag) const;

    /// Dispatches an instantaneous execution cue
    void execute_cue(const std::string& cue_tag, const GameplayCueParameters& params);

    /// Dispatches an active (start) cue
    void add_cue(const std::string& cue_tag, const GameplayCueParameters& params);

    /// Dispatches an ongoing tick update cue
    void update_cue(const std::string& cue_tag, const GameplayCueParameters& params);

    /// Dispatches a removal cue
    void remove_cue(const std::string& cue_tag, const GameplayCueParameters& params);

    /// Generic dispatch with explicit event type
    /// Invokes exact match handler and/or parent ancestor handlers if hierarchical propagation is enabled.
    void dispatch(GameplayCueType type, const std::string& cue_tag, const GameplayCueParameters& params);

    // Event recording & inspection for testing / replay
    void set_recording_enabled(bool enabled) noexcept { m_recording_enabled = enabled; }
    [[nodiscard]] bool is_recording_enabled() const noexcept { return m_recording_enabled; }
    [[nodiscard]] const std::vector<RecordedCueEvent>& recorded_events() const noexcept { return m_recorded_events; }
    void clear_recorded_events() noexcept { m_recorded_events.clear(); }

    /// Enable or disable hierarchical fallback/propagation to parent tags
    void set_hierarchical_propagation(bool enabled) noexcept { m_hierarchical = enabled; }
    [[nodiscard]] bool is_hierarchical_propagation() const noexcept { return m_hierarchical; }

private:
    std::unordered_map<std::string, std::vector<GameplayCueHandler>> m_handlers;
    std::vector<RecordedCueEvent> m_recorded_events;
    bool m_recording_enabled = false;
    bool m_hierarchical = true;
};

} // namespace nf::gameplay
