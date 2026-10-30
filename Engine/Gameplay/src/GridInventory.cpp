#include <NF/Gameplay/GridInventory.hpp>

#include <algorithm>
#include <sstream>

namespace nf::gameplay {

GridInventory::GridInventory(u32 columns, u32 rows, f32 max_weight)
    : m_columns(std::max(1u, columns))
    , m_rows(std::max(1u, rows))
    , m_max_weight(max_weight) {
    m_grid.assign(static_cast<usize>(m_columns) * m_rows, 0);
}

f32 GridInventory::total_weight() const noexcept {
    f32 total = 0.0f;
    for (const auto& item : m_items) {
        total += item.total_weight();
    }
    return total;
}

bool GridInventory::is_encumbered() const noexcept {
    return total_weight() > m_max_weight;
}

bool GridInventory::can_fit_rect(u32 x, u32 y, u32 w, u32 h, u64 ignore_instance_id) const {
    if (w == 0 || h == 0) {
        return false;
    }
    if (x + w > m_columns || y + h > m_rows) {
        return false;
    }

    for (u32 r = 0; r < h; ++r) {
        for (u32 c = 0; c < w; ++c) {
            usize idx = static_cast<usize>(y + r) * m_columns + (x + c);
            u64 occ = m_grid[idx];
            if (occ != 0 && occ != ignore_instance_id) {
                return false;
            }
        }
    }

    return true;
}

bool GridInventory::can_place_item(
    const GridItem& item,
    u32 x,
    u32 y,
    bool rotated,
    u64 ignore_instance_id
) const {
    u32 ew = rotated ? item.height : item.width;
    u32 eh = rotated ? item.width : item.height;
    return can_fit_rect(x, y, ew, eh, ignore_instance_id);
}

bool GridInventory::find_first_fit(
    u32 w,
    u32 h,
    u32& out_x,
    u32& out_y,
    bool& out_rotated,
    bool allow_rotation
) const {
    // 1. Try unrotated
    for (u32 r = 0; r < m_rows; ++r) {
        for (u32 c = 0; c < m_columns; ++c) {
            if (can_fit_rect(c, r, w, h, 0)) {
                out_x = c;
                out_y = r;
                out_rotated = false;
                return true;
            }
        }
    }

    // 2. Try rotated if allowed and dimensions differ
    if (allow_rotation && w != h) {
        for (u32 r = 0; r < m_rows; ++r) {
            for (u32 c = 0; c < m_columns; ++c) {
                if (can_fit_rect(c, r, h, w, 0)) {
                    out_x = c;
                    out_y = r;
                    out_rotated = true;
                    return true;
                }
            }
        }
    }

    return false;
}

bool GridInventory::insert_item(GridItem item, bool auto_position) {
    if (item.width == 0 || item.height == 0) {
        return false;
    }

    if (item.instance_id == 0) {
        u64 max_id = 0;
        for (const auto& it : m_items) {
            if (it.instance_id > max_id) {
                max_id = it.instance_id;
            }
        }
        item.instance_id = max_id + 1;
    } else {
        // Prevent duplicate instance ID
        if (get_item(item.instance_id) != nullptr) {
            return false;
        }
    }

    if (auto_position) {
        u32 x = 0;
        u32 y = 0;
        bool rot = false;
        if (!find_first_fit(item.width, item.height, x, y, rot, true)) {
            return false;
        }
        item.grid_x = x;
        item.grid_y = y;
        item.rotated = rot;
    } else {
        if (!can_place_item(item, item.grid_x, item.grid_y, item.rotated, 0)) {
            return false;
        }
    }

    m_items.push_back(item);
    rebuild_grid();
    return true;
}

bool GridInventory::move_item(u64 instance_id, u32 new_x, u32 new_y, bool rotated) {
    GridItem* item = get_item(instance_id);
    if (!item) {
        return false;
    }

    if (!can_place_item(*item, new_x, new_y, rotated, instance_id)) {
        return false;
    }

    item->grid_x = new_x;
    item->grid_y = new_y;
    item->rotated = rotated;
    rebuild_grid();
    return true;
}

bool GridInventory::remove_item(u64 instance_id) {
    auto it = std::find_if(m_items.begin(), m_items.end(), [instance_id](const GridItem& i) {
        return i.instance_id == instance_id;
    });

    if (it == m_items.end()) {
        return false;
    }

    m_items.erase(it);
    rebuild_grid();
    return true;
}

const GridItem* GridInventory::get_item(u64 instance_id) const {
    for (const auto& item : m_items) {
        if (item.instance_id == instance_id) {
            return &item;
        }
    }
    return nullptr;
}

GridItem* GridInventory::get_item(u64 instance_id) {
    for (auto& item : m_items) {
        if (item.instance_id == instance_id) {
            return &item;
        }
    }
    return nullptr;
}

u64 GridInventory::item_at(u32 x, u32 y) const {
    if (x >= m_columns || y >= m_rows) {
        return 0;
    }
    return m_grid[static_cast<usize>(y) * m_columns + x];
}

void GridInventory::rebuild_grid() {
    m_grid.assign(static_cast<usize>(m_columns) * m_rows, 0);

    for (const auto& item : m_items) {
        u32 ew = item.effective_width();
        u32 eh = item.effective_height();

        for (u32 r = 0; r < eh; ++r) {
            for (u32 c = 0; c < ew; ++c) {
                u32 cell_x = item.grid_x + c;
                u32 cell_y = item.grid_y + r;
                if (cell_x < m_columns && cell_y < m_rows) {
                    m_grid[static_cast<usize>(cell_y) * m_columns + cell_x] = item.instance_id;
                }
            }
        }
    }
}

void GridInventory::auto_pack() {
    if (m_items.empty()) {
        return;
    }

    // Sort items descending by footprint area, then by instance_id for determinism
    std::vector<GridItem> items_to_pack = m_items;
    std::sort(items_to_pack.begin(), items_to_pack.end(), [](const GridItem& a, const GridItem& b) {
        u32 area_a = a.width * a.height;
        u32 area_b = b.width * b.height;
        if (area_a != area_b) {
            return area_a > area_b;
        }
        return a.instance_id < b.instance_id;
    });

    clear();

    for (auto& item : items_to_pack) {
        u32 x = 0;
        u32 y = 0;
        bool rot = false;
        if (find_first_fit(item.width, item.height, x, y, rot, true)) {
            item.grid_x = x;
            item.grid_y = y;
            item.rotated = rot;
            insert_item(item, false);
        } else {
            // If doesn't fit in packed grid, restore original
            insert_item(item, true);
        }
    }
}

void GridInventory::clear() {
    m_items.clear();
    m_grid.assign(static_cast<usize>(m_columns) * m_rows, 0);
}

std::string GridInventory::serialize() const {
    std::ostringstream ss;
    ss << "GRID " << m_columns << " " << m_rows << " " << m_max_weight << "\n";
    for (const auto& it : m_items) {
        ss << "ITEM " << it.instance_id << " " << it.item_id << " "
           << it.width << " " << it.height << " "
           << it.grid_x << " " << it.grid_y << " "
           << (it.rotated ? 1 : 0) << " "
           << it.weight << " " << it.quantity << "\n";
    }
    ss << "END";
    return ss.str();
}

bool GridInventory::parse(const std::string& text) {
    std::istringstream ss(text);
    std::string tag;
    if (!(ss >> tag) || tag != "GRID") {
        return false;
    }

    u32 cols = 0;
    u32 rows = 0;
    f32 max_w = 0.0f;
    if (!(ss >> cols >> rows >> max_w)) {
        return false;
    }

    m_columns = std::max(1u, cols);
    m_rows = std::max(1u, rows);
    m_max_weight = max_w;
    clear();

    while (ss >> tag) {
        if (tag == "END") {
            break;
        }
        if (tag == "ITEM") {
            GridItem it;
            int rot_int = 0;
            if (ss >> it.instance_id >> it.item_id >> it.width >> it.height
                   >> it.grid_x >> it.grid_y >> rot_int >> it.weight >> it.quantity) {
                it.rotated = (rot_int != 0);
                m_items.push_back(it);
            }
        }
    }

    rebuild_grid();
    return true;
}

} // namespace nf::gameplay
