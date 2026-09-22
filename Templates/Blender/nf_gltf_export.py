# nf_gltf_export.py — NOVAForge one-click glTF export for Blender.
#
# Ships with the NOVAForge/SANAD Engine repo (Templates/Blender/). Install:
#   Blender > Edit > Preferences > Add-ons > Install… > pick this file,
#   then enable "Import-Export: NOVAForge glTF Export".
#
# One-click use:
#   File > Export > NOVAForge glTF (.glb)  — settings below are fixed to what
#   the engine's importer (NFModelImporter / nf::assets::import_gltf_file)
#   expects for a game-ready character (mesh + armature + actions + material).
#
# What the engine consumes (see Docs/Blender_Pipeline.md):
#   - Mesh: triangulated at export, +Y up (the exporter converts from Z-up),
#     modifiers applied, UVs from the active UV map, up to 4 bone influences.
#   - Skeleton: the armature's deform bones as a glTF skin; rest pose = the
#     armature's current pose, so export from the rest pose.
#   - Clips: every non-muted action on the armature becomes one glTF
#     animation, sampled (no curve interpolation data leaves Blender).
#   - Material: Principled BSDF base color / metallic / roughness factors.
#
# No silent substitution: if the selection has no armature or no actions, the
# export still runs (a static prop is a legitimate export), and the engine
# reports the result explicitly — skins/animations counts are printed by
# NFModelImporter and asserted by Tests/AssetTests.

bl_info = {
    "name": "NOVAForge glTF Export",
    "author": "NOVAForge/SANAD Engine",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "File > Export > NOVAForge glTF (.glb)",
    "description": "One-click glTF export tuned for the NOVAForge engine importer",
    "category": "Import-Export",
}

import bpy


def is_novaforge_scene(context):
    # A character needs at least one mesh; the armature and actions are
    # optional (props are static). This hook exists for scripted checks.
    return any(isinstance(o.data, bpy.types.Mesh) for o in context.selected_objects)


class NF_OT_export_gltf(bpy.types.Operator):
    """Export the selection as glTF for NOVAForge (character-ready settings)"""

    bl_idname = "nf.export_gltf"
    bl_label = "NOVAForge glTF (.glb)"

    filepath: bpy.props.StringProperty(subtype="FILE_PATH")

    # The settings the engine's importer needs, by the names Blender's
    # export_scene.gltf actually declares them (checked against
    # docs.blender.org/api/current/bpy.ops.export_scene.html):
    #   use_selection            (NOT export_selected_objects)
    #   export_force_sampling    (NOT export_sampling)
    #   export_def_bones         (NOT export_def_bones_only)
    # A wrong keyword makes the operator raise TypeError and the export never
    # runs, so these names are load-bearing.
    def _settings(self):
        return {
            "filepath": self.filepath,
            "export_format": "GLB",
            "export_yup": True,             # engine is Y-up; exporter converts Z-up
            "export_apply": True,           # apply modifiers (armature deform kept)
            "use_selection": True,          # only what the developer selected
            "export_animations": True,      # every non-muted action -> one clip
            "export_anim_single_armature": True,
            "export_force_sampling": True,  # bake curves to keys (LINEAR, sampled)
            "export_skins": True,
            "export_def_bones": True,       # deform bones only = the game rig
            "export_all_influences": False,  # cap at 4 weights (engine vertex binding)
            "export_morph": False,          # morph targets are not imported yet
            "export_cameras": False,
            "export_lights": False,
        }

    def execute(self, context):
        settings = self._settings()
        # Blender renames exporter options between releases. Passing one this
        # build does not know raises a TypeError and kills the whole export, so
        # drop the unknown keys — and name them in the report. Never silently:
        # a developer has to be able to see that a setting did not apply.
        supported = set(bpy.ops.export_scene.gltf.get_rna_type().properties.keys())
        dropped = sorted(k for k in settings if k not in supported)
        kwargs = {k: v for k, v in settings.items() if k in supported}
        bpy.ops.export_scene.gltf(**kwargs)
        if dropped:
            self.report({"WARNING"},
                        "NOVAForge: this Blender build has no %s; exported without it"
                        % ", ".join(dropped))
        self.report({"INFO"}, "NOVAForge: exported %s" % self.filepath)
        return {"FINISHED"}

    def invoke(self, context, _event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}


def nf_export_menu(self, _context):
    self.layout.operator(NF_OT_export_gltf.bl_idname, text="NOVAForge glTF (.glb)")


def register():
    bpy.utils.register_class(NF_OT_export_gltf)
    bpy.types.TOPBAR_MT_file_export.append(nf_export_menu)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(nf_export_menu)
    bpy.utils.unregister_class(NF_OT_export_gltf)


if __name__ == "__main__":
    register()
