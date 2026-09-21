#pragma once

// NF/Destruction/DestructibleComponent.hpp — one breakable object's live state.
//
// Design doc Section 41: "Breakable meshes / Fracture assets / Runtime
// destruction." The asset is the *shape* of how a thing comes apart; this is
// what it has been through. Two crates of the same mesh share an asset and
// carry different damage here, which is the point of cooking assets once.
//
// This is plain data with no behaviour beyond sizing its own bookkeeping, so
// the ECS stores it, the runtime steps it, and a test can construct one without
// an entity, a scene, or a physics world. The asset itself is referenced by
// index into the runtime's asset table rather than by pointer: the component
// stays trivially copyable and serialisable that way, and the fracture asset
// never has to outlive a component that mentions it.

#include <NF/Destruction/FractureAsset.hpp>
#include <NF/Core/Types.hpp>

#include <vector>

namespace nf::destruction {

struct DestructibleComponent {
    /// Slot of the cooked FractureAsset in the runtime's asset table.
    /// 0xFFFFFFFF means "not bound", and a world given such a component does
    /// nothing rather than inventing a shape.
    u32 asset_index = kInvalidChunk;

    f32 density = 1.0f;          // shard mass = chunk volume * this
    f32 strength_scale = 1.0f;   // multiplies every bond strength

    /// Impulse taken on each bond, accumulated across hits (Section 41:
    /// "impulses"). A bond that survives a blast is not healed — the next blast
    /// starts from where the first left off.
    std::vector<f32> bond_stress;

    /// One byte per bond: 1 once the bond has shattered. A byte rather than a
    /// bitfield because the state is read in the damage hot path and written
    /// once per break, and a vector<bool> would cost a dereference to read.
    std::vector<u8> bond_broken;

    /// One byte per chunk: 1 once that chunk has left the object. Indexed by
    /// chunk id, so a whole subtree can be marked in one walk. Membership, not
    /// emission: a whole region is marked when its outer bond goes, and the
    /// region is then emitted as however many intact pieces it has left.
    std::vector<u8> chunk_detached;

    /// One byte per chunk: 1 once a debris body has actually been spawned *for
    /// this chunk*. This is the flag that keeps a shard from being spawned
    /// twice: a region released after an inner bond went must not re-emit the
    /// piece that inner bond already sent flying.
    std::vector<u8> chunk_emitted;

    /// Sizes the per-bond and per-chunk state to an asset. Idempotent: state
    /// already recorded is kept, so binding the same asset twice, or re-binding
    /// after a save, does not erase the damage.
    void resize_for(const FractureAsset& asset) {
        const u8 intact = 0u;
        if (bond_stress.size() != asset.bonds.size()) {
            bond_stress.assign(asset.bonds.size(), 0.0f);
            bond_broken.assign(asset.bonds.size(), intact);
        }
        if (chunk_detached.size() != asset.chunks.size()) {
            chunk_detached.assign(asset.chunks.size(), intact);
            chunk_emitted.assign(asset.chunks.size(), intact);
        }
    }

    /// True when no bond has shattered — the object is still in one piece.
    bool intact() const {
        for (const u8 broken : bond_broken) {
            if (broken != 0u) return false;
        }
        return true;
    }

    /// Bonds shattered so far.
    u32 broken_count() const {
        u32 n = 0u;
        for (const u8 broken : bond_broken) {
            if (broken != 0u) ++n;
        }
        return n;
    }

    /// Chunks that have left the object. Every shattered bond releases a whole
    /// subtree, so this counts subtrees' worth of chunks, not single shards.
    u32 detached_count() const {
        u32 n = 0u;
        for (const u8 gone : chunk_detached) {
            if (gone != 0u) ++n;
        }
        return n;
    }

    /// Debris bodies spawned. A region that flies as one lump counts once, here
    /// and in the sink, which is what makes this comparable to active_debris().
    u32 emitted_count() const {
        u32 n = 0u;
        for (const u8 flown : chunk_emitted) {
            if (flown != 0u) ++n;
        }
        return n;
    }
};

} // namespace nf::destruction
