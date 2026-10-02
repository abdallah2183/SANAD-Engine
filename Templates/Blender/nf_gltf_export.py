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
# No silent substitution: this operator is character-specific. It refuses a
# selection without a deformed mesh, matching armature, Principled material, or
# two exportable actions. The engine CLI separately retains an explicit
# static-geometry mode; this add-on never labels a static export as a character.

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
import os


def _exportable_actions(obj):
    """Active plus non-muted NLA actions, matching Blender's ACTIONS mode."""
    actions = set()
    animation_data = obj.animation_data
    if animation_data is None:
        return actions
    if animation_data.action is not None:
        actions.add(animation_data.action)
    for track in animation_data.nla_tracks:
        if track.mute:
            continue
        for strip in track.strips:
            if not strip.mute and strip.action is not None:
                actions.add(strip.action)
    return actions


def _has_principled_material(mesh):
    for material in mesh.data.materials:
        if material is None or not material.use_nodes or material.node_tree is None:
            continue
        if any(node.type == "BSDF_PRINCIPLED" for node in material.node_tree.nodes):
            return True
    return False


def character_preflight(context):
    """Return (ok, precise failure) for the selected game character."""
    meshes = [obj for obj in context.selected_objects if obj.type == "MESH"]
    armatures = [obj for obj in context.selected_objects if obj.type == "ARMATURE"]
    if not meshes:
        return False, "select at least one character mesh"
    if not armatures:
        return False, "select the character's armature"

    deformed = any(
        modifier.type == "ARMATURE" and modifier.object == armature
        for mesh in meshes
        for modifier in mesh.modifiers
        for armature in armatures
    )
    if not deformed:
        return False, "a selected mesh needs an Armature modifier targeting a selected armature"
    if not any(_has_principled_material(mesh) for mesh in meshes):
        return False, "a selected mesh needs a material with a Principled BSDF"

    actions = set()
    for armature in armatures:
        actions.update(_exportable_actions(armature))
    if len(actions) < 2:
        return False, "the armature needs at least two active or non-muted NLA actions (found %d)" % len(actions)
    return True, "mesh + armature + deformation + material + %d actions" % len(actions)


_CRITICAL_OPTIONS = frozenset({
    "filepath",
    "export_format",
    "export_yup",
    "export_apply",
    "use_selection",
    "export_animations",
    "export_anim_single_armature",
    "export_force_sampling",
    "export_skins",
    "export_def_bones",
    "export_all_influences",
    "export_animation_mode",
    "export_influence_nb",
    "export_rest_position_armature",
})


class NF_OT_export_gltf(bpy.types.Operator):
    """Export the selection as glTF for NOVAForge (character-ready settings)"""

    bl_idname = "nf.export_gltf"
    bl_label = "NOVAForge glTF (.glb)"

    filepath: bpy.props.StringProperty(subtype="FILE_PATH")

    @classmethod
    def poll(cls, context):
        return context.scene is not None

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
            "export_animations": True,      # active + non-muted NLA actions
            "export_animation_mode": "ACTIONS",
            "export_anim_single_armature": True,
            "export_force_sampling": True,  # bake curves to LINEAR keys
            "export_skins": True,
            "export_def_bones": True,       # deform bones only = the game rig
            "export_all_influences": False,  # cap at 4 weights (engine vertex binding)
            "export_influence_nb": 4,
            "export_rest_position_armature": True,
            "export_morph": False,          # morph targets are not imported yet
            "export_cameras": False,
            "export_lights": False,
        }

    def execute(self, context):
        ok, detail = character_preflight(context)
        if not ok:
            self.report({"ERROR"}, "NOVAForge character preflight failed: %s" % detail)
            return {"CANCELLED"}

        settings = self._settings()
        supported = set(bpy.ops.export_scene.gltf.get_rna_type().properties.keys())
        missing_critical = sorted(_CRITICAL_OPTIONS - supported)
        if missing_critical:
            self.report({"ERROR"},
                        "NOVAForge: this Blender build lacks critical export options: %s"
                        % ", ".join(missing_critical))
            return {"CANCELLED"}

        # Optional options may drift between Blender releases, but a missing
        # critical option aborts rather than changing what the file contains.
        dropped = sorted(k for k in settings if k not in supported)
        kwargs = {k: v for k, v in settings.items() if k in supported}
        export_result = bpy.ops.export_scene.gltf(**kwargs)
        if "FINISHED" not in export_result:
            self.report({"ERROR"},
                        "NOVAForge: glTF exporter returned %s; no export confirmed"
                        % sorted(export_result))
            return {"CANCELLED"}

        output_path = bpy.path.abspath(self.filepath)
        if not os.path.isfile(output_path) or os.path.getsize(output_path) <= 0:
            self.report({"ERROR"},
                        "NOVAForge: exporter reported success but output is missing or empty: %s"
                        % output_path)
            return {"CANCELLED"}

        if dropped:
            self.report({"WARNING"},
                        "NOVAForge: optional settings unavailable on this Blender build: %s"
                        % ", ".join(dropped))
        self.report({"INFO"}, "NOVAForge: exported character %s (%s)" % (output_path, detail))
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
