// NF/Destruction/FractureAsset.cpp — tree queries on a built fracture asset.
// Pure data walking; no allocation beyond the caller's output vector.

#include <NF/Destruction/FractureAsset.hpp>

namespace nf::destruction {

namespace {

/// True when `ancestor` is `node` or lies on the path from `node` to the root.
/// Depth is bounded by FractureParams::max_depth, so this is a short walk.
bool is_ancestor_or_equal(const std::vector<FractureChunk>& chunks, u32 ancestor, u32 node) {
    u32 cur = node;
    while (cur != kInvalidChunk) {
        if (cur == ancestor) return true;
        cur = chunks[cur].parent;
    }
    return false;
}

} // namespace

u32 FractureAsset::leaf_count() const {
    u32 count = 0u;
    for (const FractureChunk& chunk : chunks) {
        if (chunk.is_leaf()) ++count;
    }
    return count;
}

void FractureAsset::collect_leaves(u32 chunk, std::vector<u32>& out) const {
    if (chunk >= chunks.size()) return;
    const FractureChunk& node = chunks[chunk];
    if (node.is_leaf()) {
        out.push_back(chunk);
        return;
    }
    collect_leaves(node.children[0], out);
    collect_leaves(node.children[1], out);
}

void FractureAsset::bonds_of(u32 chunk, std::vector<u32>& out) const {
    if (chunk >= chunks.size()) return;
    // A bond matters to this subtree exactly when the piece it releases is
    // inside the subtree — that includes the bond above the subtree itself,
    // which takes the whole thing off at once.
    for (usize i = 0u; i < bonds.size(); ++i) {
        if (is_ancestor_or_equal(chunks, chunk, bonds[i].detach_chunk)) {
            out.push_back(static_cast<u32>(i));
        }
    }
}

} // namespace nf::destruction
