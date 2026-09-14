# NOVAForge Editor — Product (Phase 4 + Phase 5)

## Who
A solo/team game developer iterating on `.nfscene` levels for hours on a
Windows desktop with a GPU.

## Jobs to be done
1. Open a scene and see it (viewport shows the real Runtime render).
2. Find anything fast (outliner hierarchy, asset filter).
3. Change Transform/Camera/Light/Mesh safely (inspector + undo).
4. Organize hierarchy (create/rename/reparent/delete, cycle-safe).
5. Bring in meshes (drag mesh asset → viewport creates the entity).
6. Save without fear (dirty star, Ctrl+S, errors to console, never silent loss).
7. Trust the tool (validation badge, console filters, zero leaks on exit).
8. Look-dev materials (shared PBR params + albedo, instant viewport feedback).
9. Import outside files (queue with per-job states, cooker-consistent).
10. Keep iterating while files change (hot reload meshes/textures/materials).
11. Reuse hierarchy chunks (prefabs with apply/revert, all undoable).

## Decisions
- Explicit inspector per component over generic reflection (ships now, correct now).
- One undoable command per user gesture; gizmo drags commit once on release.
- CPU ray/AABB picking first; GPU picking explicitly deferred.
- Play = snapshot + lock (no gameplay systems in v0.1, so nothing can drift).
- Keyboard: W/E/R gizmo, Ctrl+Z/Y undo/redo, Ctrl+S save, Delete request.

## Acceptance proof (automation)
`NOVAForgeEditor.exe --scene content://Scenes/Example.nfscene --frames 120 --validation`
logs: outliner labels, inspector edit + undo + redo, UI overlay proof,
play/stop, save-copy + reload round-trip, asset filter + drop + undo-drop,
material edit visible + save + undo, texture import + albedo + hot reload,
mesh import + drop + hot-reload growth, prefab create/instantiate/revert,
artifact cleanup, viewport lit pixels, `Validation errors: 0`,
`Alive RHI objects before shutdown: 0`, exit 0.

## Explicit Phase 5 limits
No physics, animation, networking, scripting, thumbnails, GPU picking, async
import workers, or OS file dialogs (logical-path inputs instead).
Rotation/scale compose into the render matrix but do not yet accumulate
through hierarchy world transforms. Texture filtering is fixed
(Linear/Clamp, no mip control in the UI). Prefab overrides are free edits
with file-level Apply/Revert (no per-property override badges).
