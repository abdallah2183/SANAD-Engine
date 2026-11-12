# Changelog

[Documentation](Docs/README.md) · [GitHub Releases](https://github.com/abdallah2183/SANAD-Engine/releases)

Release notes are kept in one place under `Docs/Releases/`. Published artifacts and source tags remain attached to their original GitHub releases.

## Unreleased

- Add deferred Lua scene load/restart/quit commands, bounded typed session data, and three-scene UiStarter flow with editor Stop restoration.
- Fix packaged project mounts resolved from relative or trailing-separator roots.

- Render custom Lua game UI in the standalone player with panels, bilingual text, buttons, bars and sliders.
- Share the native RHI overlay with MedievalVillage and package its UI shaders, Arabic font and license with projects.
- Add an independent Runtime UI Lab project and regression coverage for pointer capture, entity-scoped widget IDs and scene resets.
- Add anchored layout, wrapped right/center/left text, checkbox, scrollable list and tooltip to the scripted UI, with Tab/Enter/arrow/wheel keyboard navigation and focus outlines.
- Extend RuntimeUiLab with Main/Settings/Demo pages, visual option links and reset.
- Add measured text layout: tooltips and wrapping share the draw font's advances, over-wide words clip with an ellipsis instead of overflowing, and tooltip boxes stay on screen.
- Add `nf.ui.text_input` (Arabic/English, WM_CHAR with surrogate-pair combining, Enter/Escape, codepoint-safe editing) and a controlled `nf.ui.number_input` (locale-independent validation, red invalid status, last-valid retention).
- Add per-project local settings (`nf.settings.save/load/clear/path`) stored under the user profile with atomic writes, versioned format and size caps, isolated by project name.
- Preview `nf.ui` in the editor Game View while playing (shared `draw_display_list`, letterbox-aware input, Pause/Stop lifecycle, no disk writes on preview).
- Extend RuntimeUiLab with a Profile & Settings page (editable name, bounded integer volume, Save/Load/Restore Defaults).
- Make editor Pause a true freeze (`Runtime::set_paused`, independent of `dt`: no scripts/physics/animation/gameplay/AI/nav/audio, snapshot kept, resume discards pause time without a jump).
- Add an explicit input owner (Editor/Game/GameField) with one-frame ImGui Nav suppression so Tab/arrows move game focus once, and block pad/key/mouse gameplay while a field is editing (synthetic gamepad test).
- Add the `UiStarter` project template (main menu + `nf.settings` load-once/save-on-demand + start button), keeping `Default` unchanged.

- Add a categorized, bilingual component picker with 35 entries, duplicate protection and one-step undo for additions.
- Validate and record 2D sprite, tilemap, rigid body and collider edits through Apply/Revert drafts instead of direct world writes.
- Give each Inspector section its own property ID scope to prevent controls such as Enabled and Color from colliding across components.

- Fix overlapping Arabic system badges and distinguish ordinary audio sources from SurfaceAudio metadata.
- Preserve Outliner branch folding, indent child entities, fit long names with tooltips and route empty-space drops to their own target.

- Add bilingual component search, fold controls and a stationary search/action header to the Inspector.
- Make transform axis fields and action rows adapt to narrow panels; record each live transform drag as one undoable edit.
- Add an Outliner action menu, clearable search, visible counts and camera/inventory filters. Category filters include secondary components and descendants of folded branches.
- Organize Settings into Viewport, Rendering and Preferences tabs with a scrolling body and a stationary Close button.

- Reorganize documentation into current guides, references, release records and a historical archive.
- Refresh the repository's screenshots from 0.8.0 and remove obsolete/repeated media from the main branch.
- Add a local documentation/asset checker and clearer contribution/support navigation.

## 0.8.0

Editor reliability: synchronized panels, file browsing, undo/redo, input capture, console and import/export improvements.

[Release notes](Docs/Releases/0.8.0.md) · [Download](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.8.0)

## Earlier releases

| Version | Record | Download |
|:---|:---|:---|
| 0.7.0 | [Notes](Docs/Releases/0.7.0.md) | [GitHub](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.7.0) |
| 0.6.0 | [Notes](Docs/Releases/0.6.0.md) | [GitHub](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.6.0) |
| 0.5.0 | [Notes](Docs/Releases/0.5.0.md) | [GitHub](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.5.0) |
| 0.4.0 | [Notes](Docs/Releases/0.4.0.md) | [GitHub](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.4.0) |
| 0.3.0 | [Notes](Docs/Releases/0.3.0.md) | [GitHub](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.3.0) |
| 0.2.0 | [Notes](Docs/Releases/0.2.0.md) | [GitHub](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.2.0) |

Historical counts, screenshots and limitations describe the version named by that record, not the current branch.
