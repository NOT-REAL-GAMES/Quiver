# SPDX-License-Identifier: GPL-2.0-or-later
"""Blender entry point. Geometry extraction and file formats live separately."""

bl_info = {
    "name": "Quiver / vkQuake MD5 Exporter",
    "author": "Quiver contributors",
    "version": (0, 5, 0),
    "blender": (4, 2, 0),
    "location": "File > Export > Quiver / vkQuake MD5",
    "description": "Export MD5 meshes, PNG skins, and bind skeletons for vkQuake",
    "category": "Import-Export",
}

import bpy
import importlib
import sys
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty
from bpy.props import IntProperty, PointerProperty, StringProperty
from bpy_extras.io_utils import ExportHelper

# Blender can reload only this entry point when replacing an enabled add-on.
# Refresh our already-loaded dependencies first so the UI and backend APIs stay
# in sync, including when Blender recreated the parent package object.
for _module_name in ('model', 'geometry', 'md5', 'sizing', 'bake_cache', 'baking', 'bake_inputs',
                     'bake_manager', 'exporter'):
    _cached_module = sys.modules.get(f'{__name__}.{_module_name}')
    if _cached_module is not None:
        globals()[_module_name] = importlib.reload(_cached_module)
exporter = importlib.import_module(f'{__name__}.exporter')
ExportError = importlib.import_module(f'{__name__}.model').ExportError
bake_cache = importlib.import_module(f'{__name__}.bake_cache')


class QuiverMD5Skin(bpy.types.PropertyGroup):
    diffuse: PointerProperty(name="Diffuse", type=bpy.types.Image)
    fullbright: PointerProperty(name="Fullbright", type=bpy.types.Image,
                               description="Optional emissive mask; black pixels stay unlit")


class QuiverMD5Material(bpy.types.PropertyGroup):
    shader_name: StringProperty(name="Texture Stem", description="Optional filename stem; no extension or directory")
    bake_size_override: IntProperty(name="Bake Size Override", default=0, min=0, max=4096,
                                    description="Bake this material at exactly this size; 0 uses the export settings")
    skins: CollectionProperty(type=QuiverMD5Skin)
    active_skin: IntProperty(default=0, min=0)


class QUIVER_UL_md5_skins(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        label = item.diffuse.name if item.diffuse else "Choose a diffuse image"
        layout.label(text=f"{index}: {label}", icon='IMAGE_DATA')


class QUIVER_OT_md5_skin_add(bpy.types.Operator):
    bl_idname = "quiver.md5_skin_add"
    bl_label = "Add MD5 Skin"
    bl_options = {'UNDO', 'INTERNAL'}

    def execute(self, context):
        settings = context.material.quiver_md5
        if len(settings.skins) >= 32:
            self.report({'ERROR'}, "vkQuake supports at most 32 skins")
            return {'CANCELLED'}
        settings.skins.add()
        settings.active_skin = len(settings.skins) - 1
        return {'FINISHED'}


class QUIVER_OT_md5_skin_remove(bpy.types.Operator):
    bl_idname = "quiver.md5_skin_remove"
    bl_label = "Remove MD5 Skin"
    bl_options = {'UNDO', 'INTERNAL'}

    def execute(self, context):
        settings = context.material.quiver_md5
        if settings.skins:
            settings.skins.remove(min(settings.active_skin, len(settings.skins) - 1))
            settings.active_skin = max(0, min(settings.active_skin, len(settings.skins) - 1))
        return {'FINISHED'}


class QUIVER_OT_md5_clear_bake_cache(bpy.types.Operator):
    bl_idname = "quiver.md5_clear_bake_cache"
    bl_label = "Clear Bake Cache"
    bl_description = "Delete stored bakes so the next export bakes every surface again"
    bl_options = {'INTERNAL'}

    directory: StringProperty(name="Cache Directory", options={'HIDDEN', 'SKIP_SAVE'})

    def execute(self, context):
        root = bpy.path.abspath(self.directory) if self.directory else bake_cache.default_directory()
        try:
            removed = bake_cache.BakeCache(root).clear()
        except OSError as exc:
            self.report({'ERROR'}, f"Could not clear the bake cache: {exc}")
            return {'CANCELLED'}
        self.report({'INFO'}, f"Removed {removed} cached bakes")
        return {'FINISHED'}


class QUIVER_PT_md5_material(bpy.types.Panel):
    bl_label = "Quiver MD5 Skins"
    bl_idname = "QUIVER_PT_md5_material"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'material'

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        layout = self.layout
        settings = context.material.quiver_md5
        layout.prop(settings, "shader_name")
        layout.prop(settings, "bake_size_override")
        row = layout.row()
        row.template_list("QUIVER_UL_md5_skins", "", settings, "skins", settings, "active_skin", rows=3)
        col = row.column(align=True)
        col.operator("quiver.md5_skin_add", icon='ADD', text="")
        col.operator("quiver.md5_skin_remove", icon='REMOVE', text="")
        if settings.skins:
            skin = settings.skins[min(settings.active_skin, len(settings.skins) - 1)]
            layout.template_ID(skin, "diffuse", open="image.open")
            layout.template_ID(skin, "fullbright", open="image.open")
            layout.label(text="List entries override node textures.")
        else:
            layout.label(text="Automatic: Principled Base Color / Emission.")
            layout.label(text="Add entries for explicit skins 0, 1, 2...")


class EXPORT_SCENE_OT_quiver_md5(bpy.types.Operator, ExportHelper):
    bl_idname = "export_scene.quiver_md5"
    bl_label = "Export Quiver MD5"
    bl_options = {'PRESET'}
    filename_ext = ".md5mesh"

    filter_glob: StringProperty(default="*.md5mesh", options={'HIDDEN'})
    selected_only: BoolProperty(name="Selected Meshes Only", default=True)
    scale: FloatProperty(name="Scale", description="Game units per Blender unit", default=1, min=0.000001, max=1000000)
    forward_axis: EnumProperty(name="Blender Forward", default='NEG_Y', items=[
        ('NEG_Y', "-Y", "Blender -Y becomes Quake +X"),
        ('POS_Y', "+Y", "Blender +Y becomes Quake +X"),
        ('POS_X', "+X", "Keep Blender axes"),
        ('NEG_X', "-X", "Blender -X becomes Quake +X"),
    ])
    export_rig: BoolProperty(name="Export Armature Bind Pose", default=False,
                            description="Export rest skeleton and vertex weights; animation clips are not yet exported")
    apply_modifiers: BoolProperty(name="Apply Modifiers", default=True,
                                 description="Evaluate modifiers; armature deformation is excluded in bind pose mode")
    texture_prefix: StringProperty(name="Game Model Directory", default="progs",
                                  description="Game-relative directory where this model and its PNG skins will be installed")
    copy_textures: BoolProperty(name="Export PNG Skins", default=True,
                               description="Write diffuse and fullbright PNG images next to the MD5 files")
    bake_mode: EnumProperty(name="Material Baking", default='AUTO', items=[
        ('AUTO', "Automatic", "Bake node materials when their colors, mapping, or opacity cannot be copied directly"),
        ('ALWAYS', "Always Bake", "Bake all node materials except explicit MD5 skin lists"),
        ('OFF', "Off", "Only export directly supported image textures and constant colors"),
    ])
    bake_size_mode: EnumProperty(name="Bake Size", default='AUTO', items=[
        ('AUTO', "From Surface Area", "Size each bake from its area to keep texel density consistent"),
        ('FIXED', "Fixed", "Bake every surface at one resolution"),
    ])
    bake_texel_density: FloatProperty(name="Texel Density", default=4, min=0.01, max=1024, soft_max=32,
                                      description="Target pixels per game unit, after the export scale")
    bake_min_size: IntProperty(name="Min Size", default=64, min=32, max=4096,
                               description="Smallest automatic size; sizes are powers of two")
    bake_max_size: IntProperty(name="Max Size", default=2048, min=32, max=4096,
                               description="Largest automatic size; surfaces over it fall below the target density")
    bake_size: IntProperty(name="Bake Resolution", default=2048, min=32, max=4096,
                          description="Square skin size; 2048 for standard detail, 4096 for more detail")
    bake_margin: IntProperty(name="Bake Margin", default=8, min=0, max=64,
                            description="Extend baked colors outside UV islands by this many pixels")
    bake_samples: IntProperty(name="Bake Samples", default=16, min=1, max=256,
                             description="Cycles samples for material baking")
    bake_uv_mode: EnumProperty(name="Bake UVs", default='AUTO', items=[
        ('AUTO', "Automatic", "Generate a temporary bake layout when existing UVs are missing, tiled, or overlapping"),
        ('EXISTING', "Use Existing", "Require a non-overlapping render UV layout inside the 0–1 tile"),
    ])
    force_rebake: BoolProperty(name="Force Rebake", default=False, options={'SKIP_SAVE'},
                               description="Ignore stored bakes for this export and replace them")
    bake_cache_dir: StringProperty(name="Bake Cache", subtype='DIR_PATH',
                                   description="Where finished bakes are kept; empty uses your user cache directory")

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "selected_only")
        layout.prop(self, "scale")
        layout.prop(self, "forward_axis")
        layout.label(text="Z stays up; world origin is the model origin.")
        layout.prop(self, "apply_modifiers")
        layout.prop(self, "export_rig")
        layout.prop(self, "copy_textures")
        bake = layout.column()
        bake.enabled = self.copy_textures
        bake.prop(self, "bake_mode")
        if self.bake_mode != 'OFF':
            bake.prop(self, "bake_uv_mode")
            bake.prop(self, "bake_size_mode")
            if self.bake_size_mode == 'AUTO':
                bake.prop(self, "bake_texel_density")
                row = bake.row(align=True)
                row.prop(self, "bake_min_size")
                row.prop(self, "bake_max_size")
            else:
                bake.prop(self, "bake_size")
            bake.prop(self, "bake_margin")
            bake.prop(self, "bake_samples")
            bake.prop(self, "force_rebake")
            bake.prop(self, "bake_cache_dir")
            bake.operator(QUIVER_OT_md5_clear_bake_cache.bl_idname).directory = self.bake_cache_dir
        layout.prop(self, "texture_prefix")
        layout.label(text="Install both MD5 files and PNGs in this directory.")

    def execute(self, context):
        try:
            result = exporter.export(
                context, self.filepath, selected_only=self.selected_only, scale=self.scale,
                forward_axis=self.forward_axis, export_rig=self.export_rig,
                apply_modifiers=self.apply_modifiers, texture_prefix=self.texture_prefix,
                copy_textures=self.copy_textures,
                bake_mode=self.bake_mode, bake_size=self.bake_size,
                bake_margin=self.bake_margin, bake_samples=self.bake_samples,
                bake_uv_mode=self.bake_uv_mode, bake_size_mode=self.bake_size_mode,
                bake_texel_density=self.bake_texel_density, bake_min_size=self.bake_min_size,
                bake_max_size=self.bake_max_size, force_rebake=self.force_rebake,
                bake_cache_dir=self.bake_cache_dir,
            )
        except (ExportError, OSError, RuntimeError) as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}
        for warning in result.warnings:
            self.report({'WARNING'}, warning)
        self.report({'INFO'}, f"Exported {result.surfaces} surfaces, {result.vertices} vertices, {result.triangles} triangles")
        if result.bakes:
            for record in result.bakes:
                print(f"Quiver MD5: {record.surface}: {record.size}px {record.status}")
            counts = {status: sum(record.status == status for record in result.bakes)
                      for status in ('baked', 'cached', 'shared')}
            sizes = [record.size for record in result.bakes]
            self.report({'INFO'}, f"Bakes: {counts['baked']} baked, {counts['cached']} cached, "
                                  f"{counts['shared']} shared; {min(sizes)}-{max(sizes)} px")
        return {'FINISHED'}


_CLASSES = (QuiverMD5Skin, QuiverMD5Material, QUIVER_UL_md5_skins,
            QUIVER_OT_md5_skin_add, QUIVER_OT_md5_skin_remove, QUIVER_OT_md5_clear_bake_cache,
            QUIVER_PT_md5_material,
            EXPORT_SCENE_OT_quiver_md5)


def _export_menu(self, context):
    self.layout.operator(EXPORT_SCENE_OT_quiver_md5.bl_idname, text="Quiver / vkQuake MD5 (.md5mesh)")


def register():
    for cls in _CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Material.quiver_md5 = PointerProperty(type=QuiverMD5Material)
    bpy.types.TOPBAR_MT_file_export.append(_export_menu)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_export_menu)
    del bpy.types.Material.quiver_md5
    for cls in reversed(_CLASSES):
        bpy.utils.unregister_class(cls)
