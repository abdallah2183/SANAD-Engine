#pragma once

#include <NF/Core/Types.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace nf::gameplay {

/// An item placed inside a 2D spatial grid inventory (e.g. Diablo/Tarkov style)
struct GridItem {
    u64 instance_id = 0;             ///< Unique instance identifier (non-zero)
    std::string item_id;             ///< Prototype item definition identifier
    u32 width = 1;                   ///< Base width in grid cells
    u32 height = 1;                  ///< Base height in grid cells
    u32 grid_x = 0;                  ///< Top-left column coordinate in grid
    u32 grid_y = 0;                  ///< Top-left row coordinate in grid
    bool rotated = false;            ///< If true, width and height are swapped
    f32 weight = 0.0f;               ///< Weight per item unit (kg)
    u32 quantity = 1;                ///< Stack count

    [[nodiscard]] u32 effective_width() const noexcept {
        return rotated ? height : width;
    }

    [[nodiscard]] u32 effective_height() const noexcept {
        return rotated ? width : height;
    }

    [[nodiscard]] f32 total_weight() const noexcept {
        return weight * static_cast<f32>(quantity);
    }
};

/// 2D spatial grid inventory with rotation, collision detection, and auto-packing
class GridInventory {
public:
    GridInventory(u32 columns = 10, u32 rows = 6, f32 max_weight = 50.0f);

    [[nodiscard]] u32 columns() const noexcept { return m_columns; }
    [[nodiscard]] u32 rows() const noexcept { return m_rows; }
    [[nodiscard]] f32 max_weight() const noexcept { return m_max_weight; }
    void set_max_weight(f32 max_w) noexcept { m_max_weight = max_w; }

    /// Total weight of all items currently stored
    [[nodiscard]] f32 total_weight() const noexcept;

    /// Whether total weight exceeds maximum capacity
    [[nodiscard]] bool is_encumbered() const noexcept;

    /// Number of distinct item instances
    [[nodiscard]] usize item_count() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }

    /// Read-only access to all items
    [[nodiscard]] const std::vector<GridItem>& items() const noexcept { return m_items; }

    /// Check if a rectangle of size (w, h) can fit at (x, y) without colliding with other items
    [[nodiscard]] bool can_fit_rect(u32 x, u32 y, u32 w, u32 h, u64 ignore_instance_id = 0) const;

    /// Check if item can be placed at (x, y) with specified rotation
    [[nodiscard]] bool can_place_item(const GridItem& item, u32 x, u32 y, bool rotated, u64 ignore_instance_id = 0) const;

    /// Find first vacant cell that can fit rectangle (w, h). Returns true and sets out_x/out_y/out_rotated if found.
    [[nodiscard]] bool find_first_fit(u32 w, u32 h, u32& out_x, u32& out_y, bool& out_rotated, bool allow_rotation = true) const;

    /// Insert item into grid. If auto_position is true, automatically finds the first vacant slot.
    /// Returns true on success, false if no space or item invalid.
    bool insert_item(GridItem item, bool auto_position = true);

    /// Move existing item to new coordinates and rotation
    bool move_item(u64 instance_id, u32 new_x, u32 new_y, bool rotated);

    /// Remove item by instance ID. Returns true if found and removed.
    bool remove_item(u64 instance_id);

    /// Find item by instance ID
    [[nodiscard]] const GridItem* get_item(u64 instance_id) const;
    [[nodiscard]] GridItem* get_item(u64 instance_id);

    /// Get instance ID of item occupying cell (x, y) (returns 0 if empty)
    [[nodiscard]] u64 item_at(u32 x, u32 y) const;

    /// Rebuild internal occupancy grid (automatically called upon modifications)
    void rebuild_grid();

    /// Automatically reorganize items to maximize free contiguous space (heuristic pack)
    void auto_pack();

    /// Remove all items
    void clear();

    /// Serialize inventory to text format
    [[nodiscard]] std::string serialize() const;

    /// Parse inventory from text format
    bool parse(const std::string& text);

private:
    u32 m_columns = 10;
    u32 m_rows = 6;
    f32 m_max_weight = 50.0f;

    std::vector<GridItem> m_items;
    std::vector<u64> m_grid; // m_rows * m_columns, holds instance_id or 0
};

} // namespace nf::gameplay
