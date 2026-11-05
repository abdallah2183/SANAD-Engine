#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/AssetRename.hpp>

NF_TEST(editor_asset_rename_requires_a_single_valid_name) {
    using nf::editor::valid_asset_rename_leaf;
    NF_CHECK(valid_asset_rename_leaf("stone.nfmesh"));
    NF_CHECK(valid_asset_rename_leaf("مشهد.nfscene"));
    NF_CHECK(!valid_asset_rename_leaf(""));
    NF_CHECK(!valid_asset_rename_leaf("."));
    NF_CHECK(!valid_asset_rename_leaf(".."));
    NF_CHECK(!valid_asset_rename_leaf("../other.nfscene"));
    NF_CHECK(!valid_asset_rename_leaf("folder\\file.nfscene"));
    NF_CHECK(!valid_asset_rename_leaf("name:other"));
    NF_CHECK(!valid_asset_rename_leaf("bad?name"));
    NF_CHECK(!valid_asset_rename_leaf("trailing."));
    NF_CHECK(!valid_asset_rename_leaf("trailing "));
}
