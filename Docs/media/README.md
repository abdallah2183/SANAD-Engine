# Media policy and provenance

[Documentation](../README.md) · [Screenshot gallery](../Screenshots.md)

## Current captures

Screenshots were recorded from native **SANAD 0.8.0** on Windows on **2026-11-05**. PNGs are stored once in `Docs/images/`; the README, guides and website refer to those files.

| File | Context |
|:---|:---|
| [`shot_editor_en.png`](../images/shot_editor_en.png) | English editor, TreeWorld scene and Camera panel |
| [`shot_editor_ar.png`](../images/shot_editor_ar.png) | Arabic editor, TreeWorld scene and Camera panel |
| [`editor_lighting_ar.png`](../images/editor_lighting_ar.png) | Arabic Lighting panel |
| [`editor_input_ar.png`](../images/editor_input_ar.png) | Arabic Input panel with search and save controls |
| [`shot_game.png`](../images/shot_game.png) | ThirdPerson project opened in the current editor; launcher thumbnail |
| [`shot_vehicle.png`](../images/shot_vehicle.png) | Vehicle scene opened in the current editor; launcher thumbnail |

`shot_game.png`, `shot_vehicle.png`, `shot_editor_en.png` and `shot_editor_ar.png` are required by the native launcher's thumbnail lookup and portable packager. Keep those filenames unless the lookup and packaging contract change together.

The canonical editor mark is [`Editor/resources/SANAD.png`](../../Editor/resources/SANAD.png). The existing branding artwork is kept once at [`Resources/Branding/sanad_logo.jpg`](../../Resources/Branding/sanad_logo.jpg).

## Add or replace a screenshot

1. Use the released/native application and a real scene; do not present an AI image or a mockup as running engine output.
2. Use a bounded run with `--no-automation --no-start-here --screenshot <file.bmp>` for an unchanged scene capture.
3. Choose the intended UI language. Use an isolated settings directory for automated captures rather than overwriting a user's preferences.
4. Convert the capture to PNG without altering its content. Review the result for clipped UI, private paths and misleading captions.
5. Update this manifest and the gallery, then run `python Scripts/check_docs.py`.

Keep temporary BMPs, crops, logs and alternate takes in an ignored working folder. Older demo media and development screenshots remain in Git history/release tags, rather than the current documentation surface.
