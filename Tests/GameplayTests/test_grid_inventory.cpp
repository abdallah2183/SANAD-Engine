// Tests/GameplayTests/test_grid_inventory.cpp — Spatial 2D Grid Inventory tests (Phase 117)

#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/GridInventory.hpp>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(grid_inv_empty_init) {
    GridInventory inv(8, 4, 30.0f);
    NF_CHECK_EQ(inv.columns(), 8u);
    NF_CHECK_EQ(inv.rows(), 4u);
    NF_CHECK_NEAR(inv.max_weight(), 30.0f, 1e-4f);
    NF_CHECK(inv.empty());
    NF_CHECK_EQ(inv.item_count(), 0u);
    NF_CHECK_NEAR(inv.total_weight(), 0.0f, 1e-4f);
    NF_CHECK(!inv.is_encumbered());
}

NF_TEST(grid_inv_insert_single_item) {
    GridInventory inv(6, 6, 50.0f);

    GridItem item;
    item.instance_id = 101;
    item.item_id = "dagger";
    item.width = 1;
    item.height = 2;
    item.weight = 0.5f;

    bool ok = inv.insert_item(item, true);
    NF_CHECK(ok);
    NF_CHECK_EQ(inv.item_count(), 1u);
    NF_CHECK_EQ(inv.item_at(0, 0), 101u);
    NF_CHECK_EQ(inv.item_at(0, 1), 101u);
    NF_CHECK_EQ(inv.item_at(0, 2), 0u); // adjacent cell empty
}

NF_TEST(grid_inv_boundary_overflow_rejected) {
    GridInventory inv(3, 3, 50.0f);

    GridItem item;
    item.instance_id = 1;
    item.item_id = "large_chest";
    item.width = 2;
    item.height = 2;
    item.grid_x = 2; // (2, 2) + size (2, 2) -> exceeds width 3 and height 3!
    item.grid_y = 2;

    bool ok = inv.insert_item(item, false);
    NF_CHECK(!ok);
    NF_CHECK(inv.empty());
}

NF_TEST(grid_inv_collision_rejection) {
    GridInventory inv(4, 4, 50.0f);

    GridItem item1;
    item1.instance_id = 1;
    item1.width = 2;
    item1.height = 2;
    item1.grid_x = 0;
    item1.grid_y = 0;
    NF_CHECK(inv.insert_item(item1, false));

    GridItem item2;
    item2.instance_id = 2;
    item2.width = 2;
    item2.height = 2;
    item2.grid_x = 1; // overlaps with item 1 at (1, 0), (1, 1)
    item2.grid_y = 1;
    NF_CHECK(!inv.insert_item(item2, false));

    NF_CHECK_EQ(inv.item_count(), 1u);
}

NF_TEST(grid_inv_find_first_fit_sequential) {
    GridInventory inv(4, 4, 50.0f);

    // Insert four 2x2 items to perfectly tile the 4x4 grid
    for (u64 id = 1; id <= 4; ++id) {
        GridItem it;
        it.instance_id = id;
        it.width = 2;
        it.height = 2;
        NF_CHECK(inv.insert_item(it, true));
    }

    NF_CHECK_EQ(inv.item_count(), 4u);

    // 5th item cannot fit
    GridItem extra;
    extra.instance_id = 5;
    extra.width = 1;
    extra.height = 1;
    NF_CHECK(!inv.insert_item(extra, true));
}

NF_TEST(grid_inv_item_rotation) {
    GridInventory inv(1, 3, 50.0f); // 1 column, 3 rows

    GridItem item;
    item.instance_id = 1;
    item.width = 3;  // base width 3, height 1
    item.height = 1;

    // Unrotated (3x1) cannot fit into (1x3)
    NF_CHECK(!inv.can_fit_rect(0, 0, 3, 1, 0));

    // Rotated (1x3) fits into (1x3)
    NF_CHECK(inv.can_fit_rect(0, 0, 1, 3, 0));

    // Auto-position should automatically rotate the item
    bool ok = inv.insert_item(item, true);
    NF_CHECK(ok);

    const GridItem* inserted = inv.get_item(1);
    NF_CHECK(inserted != nullptr);
    NF_CHECK(inserted->rotated);
    NF_CHECK_EQ(inserted->effective_width(), 1u);
    NF_CHECK_EQ(inserted->effective_height(), 3u);
}

NF_TEST(grid_inv_move_item) {
    GridInventory inv(6, 6, 50.0f);

    GridItem item;
    item.instance_id = 7;
    item.width = 2;
    item.height = 1;
    inv.insert_item(item, true); // placed at (0, 0)
    NF_CHECK_EQ(inv.item_at(0, 0), 7u);
    NF_CHECK_EQ(inv.item_at(1, 0), 7u);

    // Move to (3, 3)
    bool moved = inv.move_item(7, 3, 3, false);
    NF_CHECK(moved);
    NF_CHECK_EQ(inv.item_at(0, 0), 0u);
    NF_CHECK_EQ(inv.item_at(1, 0), 0u);
    NF_CHECK_EQ(inv.item_at(3, 3), 7u);
    NF_CHECK_EQ(inv.item_at(4, 3), 7u);
}

NF_TEST(grid_inv_remove_item) {
    GridInventory inv(4, 4, 50.0f);

    GridItem item;
    item.instance_id = 42;
    item.width = 2;
    item.height = 2;
    item.weight = 3.5f;
    inv.insert_item(item, true);
    NF_CHECK_EQ(inv.item_count(), 1u);
    NF_CHECK_NEAR(inv.total_weight(), 3.5f, 1e-4f);

    bool removed = inv.remove_item(42);
    NF_CHECK(removed);
    NF_CHECK(inv.empty());
    NF_CHECK_NEAR(inv.total_weight(), 0.0f, 1e-4f);
    NF_CHECK_EQ(inv.item_at(0, 0), 0u);
}

NF_TEST(grid_inv_weight_and_encumbrance) {
    GridInventory inv(10, 10, 10.0f);

    GridItem potion;
    potion.instance_id = 1;
    potion.weight = 2.0f;
    potion.quantity = 4; // 2.0 * 4 = 8.0 kg
    inv.insert_item(potion, true);

    NF_CHECK_NEAR(inv.total_weight(), 8.0f, 1e-4f);
    NF_CHECK(!inv.is_encumbered());

    GridItem rock;
    rock.instance_id = 2;
    rock.weight = 5.0f;
    rock.quantity = 1; // + 5.0 kg = 13.0 kg (> 10.0 kg max)
    inv.insert_item(rock, true);

    NF_CHECK_NEAR(inv.total_weight(), 13.0f, 1e-4f);
    NF_CHECK(inv.is_encumbered());
}

NF_TEST(grid_inv_serialize_and_parse_roundtrip) {
    GridInventory inv(8, 6, 45.0f);

    GridItem rifle;
    rifle.instance_id = 100;
    rifle.item_id = "assault_rifle";
    rifle.width = 4;
    rifle.height = 2;
    rifle.weight = 4.2f;
    rifle.quantity = 1;
    inv.insert_item(rifle, true);

    GridItem ammo;
    ammo.instance_id = 101;
    ammo.item_id = "ammo_556";
    ammo.width = 1;
    ammo.height = 1;
    ammo.weight = 0.3f;
    ammo.quantity = 30;
    inv.insert_item(ammo, true);

    std::string text = inv.serialize();
    NF_CHECK(!text.empty());

    GridInventory restored(1, 1, 1.0f);
    bool ok = restored.parse(text);
    NF_CHECK(ok);

    NF_CHECK_EQ(restored.columns(), 8u);
    NF_CHECK_EQ(restored.rows(), 6u);
    NF_CHECK_NEAR(restored.max_weight(), 45.0f, 1e-4f);
    NF_CHECK_EQ(restored.item_count(), 2u);

    const GridItem* r_rifle = restored.get_item(100);
    NF_CHECK(r_rifle != nullptr);
    NF_CHECK_EQ(r_rifle->item_id, "assault_rifle");
    NF_CHECK_EQ(r_rifle->width, 4u);
    NF_CHECK_EQ(r_rifle->height, 2u);
    NF_CHECK_NEAR(r_rifle->weight, 4.2f, 1e-4f);

    const GridItem* r_ammo = restored.get_item(101);
    NF_CHECK(r_ammo != nullptr);
    NF_CHECK_EQ(r_ammo->quantity, 30u);
}
