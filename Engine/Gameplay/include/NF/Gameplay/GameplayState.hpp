#pragma once

// NF/Gameplay/GameplayState.hpp — module state <-> text map (Phase 10, W2)
//
// The bridge between a module's reflected struct and the key/value map a scene
// file can hold. Kept separate from Components.hpp so the ECS struct stays a
// plain data type and the marshalling rules live in one place.

#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModule.hpp>

#include <string>
#include <unordered_map>

namespace nf::gameplay {

/// Copies every `SerializeField` property of `binding` into `out`, replacing
/// any previous contents. Properties without the flag are deliberately skipped:
/// `EditAnywhere` means "the inspector may touch it", which is not the same as
/// "it belongs in a save file".
///
/// Returns false when the binding is invalid, leaving `out` empty.
bool capture_state(const GameplayStateBinding& binding,
                   std::unordered_map<std::string, std::string>& out);

/// Reads `in` into the binding's reflected properties.
///
/// Keys the class does not declare are ignored, and a malformed value is skipped
/// without touching the member, so a corrupt save cannot half-apply. Returns how
/// many properties were actually written, which is the observable a test wants:
/// "the load reported success" and "the load changed something" are different
/// claims.
u32 apply_state(const GameplayStateBinding& binding,
                const std::unordered_map<std::string, std::string>& in);

/// Encodes a property map as one space-free token: `name=value|name=value`.
///
/// The scene format is a run of space-separated `key=value` tokens, and module
/// state is user data — a string property may contain spaces, `|` and `=`.
/// The escaping lives here rather than in a serializer so that the scene file
/// and the save file cannot end up disagreeing about what a value means, which
/// would be a silent corruption rather than a visible failure.
///
/// Escapes: `\` -> `\\`, `|` -> `\p`, space -> `\s`, and the control characters
/// as `\n` / `\r` / `\t`. `=` needs no escape: a pair splits on its first `=`.
std::string encode_properties(const std::unordered_map<std::string, std::string>& properties);

/// Inverse of `encode_properties`. Entries with no `=` are skipped, and an
/// unknown escape is kept verbatim so a value written by a newer engine is not
/// silently mangled by an older one.
void decode_properties(std::string_view encoded,
                       std::unordered_map<std::string, std::string>& out);

} // namespace nf::gameplay
