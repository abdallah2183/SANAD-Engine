# NOVAForge Editor — Design (Phase 4 + Phase 5)

## Identity
Dense desktop tool UI for long sessions. Dark graphite (`#26272B`
windows), one restrained blue accent (`#4C8DFF` at low alpha) reserved for
selection and primary actions. System font first (Segoe UI on Windows,
ImGui default fallback). High density, fixed 2px rounding, square windows.

## Non-goals (explicit)
No gradients, glassmorphism, or decorative animation. Motion exists only as
150–200ms panel open/selection transitions; ImGui ships with no animation by
default and none is added. No thumbnails, no scripting UI.

## Layout (docking, never floating chaos)
- Top toolbar: New / Open / Save / Play / Stop, validation badge, FPS, scene label + dirty star.
- Left: Scene Outliner (hierarchy tree, inline delete confirm).
- Center: Viewport (offscreen Runtime target; stats + gizmo mode line).
- Right: Inspector (explicit per-component sections + Apply).
- Bottom: Assets browser + Console tabs.
- All panels dock into one root `DockSpaceOverViewport(PassthruCentralNode)`;
  layout is deterministic (`IniFilename = nullptr`).

## State rules
- Selection is Entity id+generation, pruned after every structural op.
- Every mutation is a validated Command (invalid input → console error, scene untouched).
- Delete uses inline confirm, never a modal by default.
- Play snapshots the edit scene and locks structural edits; Stop restores the guarantee.

## Rendering split (Phase 5: UI composes over the scene)
- Viewport pixels: `Runtime::render_offscreen()` into a resize-safe target
  (proven by readback: "viewport lit=" log line).
- Window present: `Runtime::render()` to the swapchain (same scene), then a
  UI overlay pass (`ColorLoad::Load`, blend on) records `UiRenderer` — an
  RHI-only ImGui draw backend (no raw Vulkan outside the RHI) — over it.
  A same-usage color memory barrier between the passes makes the Load
  well-defined on all GPUs, not just coherent desktop ones.
- The viewport panel samples the offscreen target live (`Image()`), so what
  you see in the panel is the same render the readback proves.
- UI-over-scene proof: automation composites the same draw data offscreen
  and requires >5000 differing pixels ("UI overlay drew over scene").

## Shared material assets (Phase 5)
- `MeshComponent::material` names a `.nfmat` path; one renderer instance
  (one 48-byte UBO) per path, shared by all assigned entities. Param edits
  rewrite the UBO in place — visible next frame, pipelines untouched.
- Albedo is an optional content image bound to the instance; missing files
  stay scalar, never fatal.
- Unsaved UI edits always win over disk (hot reload skips dirty entries).

## Import / hot reload / prefabs (Phase 5)
- Import: validate -> copy -> register through the cooker's pipeline,
  synchronous and deterministic (one job/frame in the shell).
- Hot reload is poll-based (no OS APIs): meshes rebuild live in place
  (handles stable), textures re-upload + rebind, materials re-read unless
  dirty. Scenes are never auto-reloaded.
- A prefab IS a `.nfscene` file; instances are linked subtrees with free
  local overrides; Apply pushes, Revert replaces, both undoable.
