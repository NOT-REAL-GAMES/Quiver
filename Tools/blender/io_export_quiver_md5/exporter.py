# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract Blender data into the format-neutral model, then publish an MD5 bundle.

Static meshes bake the evaluated scene. Rigged meshes use rest geometry and
joint-local weights. Neither path changes selection, poses, or source images.
"""

from array import array
from dataclasses import dataclass, replace
from contextlib import ExitStack
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tempfile
import zlib

import bpy
from mathutils import Matrix

from .model import ExportError, Joint, Model, Surface, Vertex, Weight, split_surface
from .geometry import clean_degenerate_faces, triangle_has_area
from . import bake_cache, md5
from .sizing import DEFAULT_DENSITY, SizePolicy


@dataclass(frozen=True)
class ExportResult:
    filepath: str
    surfaces: int
    vertices: int
    triangles: int
    textures: int
    warnings: tuple
    bakes: tuple = ()  # One bake_manager.BakeRecord per baked surface.


@dataclass(frozen=True)
class _Texture:
    image: object = None
    color: tuple = None
    alpha: float | None = None
    path: Path = None  # A finished PNG, published byte for byte.


class NeedsBaking(ExportError):
    """A material channel needs evaluation instead of direct image copying."""


def _prefix(value):
    value = value.strip().replace('\\', '/')
    if not value:
        return ''
    if (value.startswith('/') or value.endswith('/') or
            any(p in ('', '.', '..') for p in value.split('/')) or
            not re.fullmatch(r'[A-Za-z0-9_/-]+', value)):
        raise ExportError("Game Model Directory must be a relative game path, for example progs/props")
    return str(PurePosixPath(value))


def _slug(value):
    return re.sub(r'[^a-z0-9_-]+', '_', value.lower()).strip('_') or 'model'


def _conversion(scale, forward_axis):
    if not math.isfinite(scale) or scale <= 0:
        raise ExportError("Scale must be finite and greater than zero")
    angles = {'POS_X': 0, 'NEG_X': math.pi, 'POS_Y': -math.pi / 2, 'NEG_Y': math.pi / 2}
    if forward_axis not in angles:
        raise ExportError("Unsupported forward axis")
    return Matrix.Rotation(angles[forward_axis], 4, 'Z') @ Matrix.Scale(scale, 4)


def _rig_for(objects):
    rigs = []
    for obj in objects:
        if obj.parent_type == 'BONE':
            raise ExportError(f"{obj.name}: use an Armature modifier instead of bone parenting for bind export")
        modifiers = [m for m in obj.modifiers if m.type == 'ARMATURE' and m.show_viewport and m.object]
        if len(modifiers) != 1:
            raise ExportError(f"{obj.name}: bind export needs exactly one enabled Armature modifier")
        mod = modifiers[0]
        if not mod.use_vertex_groups or mod.use_bone_envelopes or mod.use_deform_preserve_volume:
            raise ExportError(f"{obj.name}: use vertex groups with envelopes and Preserve Volume disabled for MD5")
        if mod.vertex_group or mod.use_multi_modifier:
            raise ExportError(f"{obj.name}: masked and multi-modifier armatures are not supported by MD5 bind export")
        rigs.append(mod.object)
    if any(rig != rigs[0] for rig in rigs):
        raise ExportError("All exported meshes must share one armature")
    return rigs[0]


def _joint_matrix(matrix, name):
    basis = matrix.to_3x3()
    columns = [basis.col[i].copy() for i in range(3)]
    lengths = [c.length for c in columns]
    if (min(lengths) < 1e-8 or basis.determinant() <= 0 or
            max(lengths) - min(lengths) > max(lengths) * 1e-5 or
            any(abs(columns[a].dot(columns[b])) > lengths[a] * lengths[b] * 1e-5
                for a, b in ((0, 1), (0, 2), (1, 2)))):
        raise ExportError(f"Joint {name}: MD5 requires rotation and uniform positive scale; apply armature scale/shear first")
    rotation = basis.to_quaternion().normalized()
    if rotation.w > 0:
        rotation.negate()
    rigid = rotation.to_matrix().to_4x4()
    rigid.translation = matrix.translation
    return rigid, tuple(rotation)[1:]


def _skeleton(rig, conversion):
    bones = []

    def visit(bone):
        bones.append(bone)
        for child in sorted(bone.children, key=lambda b: b.name):
            visit(child)

    for bone in sorted((b for b in rig.data.bones if b.parent is None), key=lambda b: b.name):
        visit(bone)
    if not bones or len(bones) > 256:
        raise ExportError("An MD5 armature must have between 1 and 256 bones")
    indices = {bone.name: i for i, bone in enumerate(bones)}
    joints, inverses = [], []
    for bone in bones:
        rigid, orientation = _joint_matrix(conversion @ rig.matrix_world @ bone.matrix_local, bone.name)
        parent = indices[bone.parent.name] if bone.parent else -1
        joints.append(Joint(bone.name, parent, tuple(rigid.translation), orientation))
        inverses.append(rigid.inverted())
    deform = {b.name: indices[b.name] for b in bones if b.use_deform}
    return tuple(joints), inverses, deform


def _weights(obj, vertex, position, inverses, groups, warnings):
    influences = [(groups[g.group], float(g.weight)) for g in vertex.groups
                  if g.group in groups and g.weight > 0]
    if any(not math.isfinite(w) for _, w in influences):
        raise ExportError(f"{obj.name}: vertex {vertex.index} has non-finite weights")
    influences.sort(key=lambda pair: (-pair[1], pair[0]))
    if len(influences) > 4:
        warnings.add(f"{obj.name}: reduced vertex influences to the strongest four and normalized weights")
        influences = influences[:4]
    total = sum(w for _, w in influences)
    if total <= 1e-12:
        raise ExportError(f"{obj.name}: vertex {vertex.index} has no deform-bone weights")
    return tuple(Weight(joint, weight / total, tuple(inverses[joint] @ position))
                 for joint, weight in influences)


def _source_node(socket):
    seen = set()
    while socket.is_linked:
        link = socket.links[0]
        node = link.from_node
        if node.type != 'REROUTE':
            return node, link.from_socket
        if node.as_pointer() in seen:
            raise ExportError("Material has a cyclic node connection")
        seen.add(node.as_pointer())
        socket = node.inputs[0]
    return None, None


def _image_from_socket(socket, material, uv_name):
    node, output = _source_node(socket)
    if node is None:
        return None
    if node.type != 'TEX_IMAGE' or output.name != 'Color' or node.image is None:
        raise NeedsBaking(f"{material.name}: {socket.name} needs material baking; enable Automatic baking")
    if node.projection != 'FLAT' or node.extension != 'REPEAT':
        raise NeedsBaking(f"{material.name}: image projection or extension needs material baking")
    vector, vector_output = _source_node(node.inputs['Vector'])
    if vector is not None:
        valid_uv = (vector.type == 'UVMAP' and (not vector.uv_map or vector.uv_map == uv_name))
        valid_uv |= vector.type == 'TEX_COORD' and vector_output.name == 'UV'
        if not valid_uv:
            raise NeedsBaking(f"{material.name}: alternate UV maps or Mapping nodes need material baking")
    return node.image


def _validate_image(image):
    if image.source not in {'FILE', 'GENERATED'} or (image.source == 'FILE' and image.type != 'IMAGE'):
        raise NeedsBaking(f"{image.name}: this image source needs material baking or a single PNG skin")
    if min(image.size) <= 0 or len(image.pixels) == 0:
        raise ExportError(f"{image.name}: image pixels are unavailable; reload or pack the image")


def _material_skins(material, uv_name):
    if material and material.quiver_md5.skins:
        skins = []
        for i, skin in enumerate(material.quiver_md5.skins):
            if skin.diffuse is None:
                raise ExportError(f"{material.name}: skin {i} needs a diffuse image")
            skins.append((_Texture(image=skin.diffuse), _Texture(image=skin.fullbright) if skin.fullbright else None))
    else:
        color = tuple(material.diffuse_color) if material else (0.8, 0.8, 0.8, 1.0)
        diffuse, fullbright, alpha_override = None, None, None
        if material and material.use_nodes:
            outputs = [n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL' and n.is_active_output]
            shader, _ = _source_node(outputs[0].inputs['Surface']) if outputs else (None, None)
            if shader is None or shader.type != 'BSDF_PRINCIPLED':
                raise NeedsBaking(f"{material.name}: this surface shader needs material baking or explicit MD5 skins")
            base = shader.inputs['Base Color']
            diffuse = _image_from_socket(base, material, uv_name)
            color = tuple(base.default_value)
            alpha = shader.inputs['Alpha']
            if alpha.is_linked:
                base_node, _ = _source_node(base)
                alpha_node, alpha_output = _source_node(alpha)
                if not diffuse or alpha_node != base_node or alpha_output.name != 'Alpha':
                    raise NeedsBaking(f"{material.name}: linked opacity needs material baking")
            else:
                alpha_override = float(alpha.default_value)
                color = (*color[:3], alpha_override)
            emission = shader.inputs.get('Emission Color') or shader.inputs.get('Emission')
            strength = shader.inputs.get('Emission Strength')
            if emission and (strength is None or strength.is_linked or strength.default_value != 0):
                if strength and strength.is_linked:
                    raise NeedsBaking(f"{material.name}: linked emission strength needs material baking")
                glow_image = _image_from_socket(emission, material, uv_name)
                if glow_image:
                    if strength and strength.default_value != 1:
                        raise NeedsBaking(f"{material.name}: emission intensity needs material baking")
                    fullbright = _Texture(image=glow_image, alpha=1.0)
                elif any(v > 0 for v in emission.default_value[:3]):
                    intensity = strength.default_value if strength else 1
                    fullbright = _Texture(color=tuple(v * intensity for v in emission.default_value[:3]) + (1.0,))
        skins = [(_Texture(image=diffuse, alpha=alpha_override) if diffuse else _Texture(color=color), fullbright)]
    if not 1 <= len(skins) <= 32:
        raise ExportError("vkQuake supports 1 to 32 contiguous skins per surface")
    for pair in skins:
        for texture in pair:
            if texture and texture.image:
                _validate_image(texture.image)
    return tuple(skins)


def _skin_stem(filepath, material, index, prefix):
    override = material.quiver_md5.shader_name.strip() if material else ''
    if override:
        if not re.fullmatch(r'[A-Za-z0-9_-]+', override):
            raise ExportError(f"{material.name}: Texture Stem must contain only letters, digits, underscores, or hyphens")
        basename = override
    else:
        available = 48 - len(prefix) - (1 if prefix else 0)
        suffix = f'_{index:02d}'
        if available <= len(suffix):
            raise ExportError("Game Model Directory is too long for Quake texture paths")
        basename = _slug(filepath.stem)[:available - len(suffix)] + suffix
    shader = f'{prefix}/{basename}' if prefix else basename
    return shader, basename


def _extract(context, objects, filepath, conversion, rig, apply_modifiers, prefix, warnings, material_skins):
    if rig:
        joints, inverses, deform = _skeleton(rig, conversion)
    else:
        joints, inverses, deform = (Joint('origin', -1, (0, 0, 0), (0, 0, 0)),), [], {}
    surfaces, texture_files, skin_counts = [], {}, []
    depsgraph = context.evaluated_depsgraph_get()
    for obj in objects:
        groups = {g.index: deform[g.name] for g in obj.vertex_groups if g.name in deform}
        matrix = conversion @ obj.matrix_world
        determinant = matrix.to_3x3().determinant()
        if abs(determinant) < 1e-12:
            raise ExportError(f"{obj.name}: object has a singular transform")
        evaluated = obj.evaluated_get(depsgraph) if apply_modifiers else None
        mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph) if evaluated else obj.data.copy()
        try:
            removed = clean_degenerate_faces(mesh)
            if removed:
                warnings.add(f"{obj.name}: removed {removed} degenerate face(s) from the export copy")
            if not mesh.polygons:
                raise ExportError(f"{obj.name}: mesh has no non-degenerate faces to export")
            mesh.calc_loop_triangles()
            # Unconnected image Vector inputs and Texture Coordinate UV nodes
            # use the render UV map, which can differ from the editing map.
            uv_layer = next((layer for layer in mesh.uv_layers if layer.active_render), mesh.uv_layers.active)
            by_material = {}
            for triangle in mesh.loop_triangles:
                if triangle_has_area(mesh, triangle):
                    by_material.setdefault(triangle.material_index, []).append(triangle)
            for material_index, triangles in sorted(by_material.items()):
                material_object = evaluated if evaluated else obj
                material = (material_object.material_slots[material_index].material if material_index < len(material_object.material_slots)
                            else mesh.materials[material_index] if material_index < len(mesh.materials) else None)
                # Evaluated slots include modifier-assigned materials, but their
                # copied custom skin properties may lag edits made since the
                # last dependency-graph update. Read live settings from the ID.
                material = material.original if material else None
                skins, baked_uvs = material_skins(obj, mesh, material_index, material,
                                                  uv_layer.name if uv_layer else None)
                if baked_uvs is not None and len(baked_uvs) != len(mesh.loops):
                    raise ExportError(f"{obj.name}: generated bake UVs do not match exported mesh topology")
                skin_counts.append(len(skins))
                shader, basename = _skin_stem(filepath, material, len(surfaces), prefix)
                for skin_index, (diffuse, glow) in enumerate(skins):
                    for suffix, texture in (('', diffuse), ('_glow', glow)):
                        if texture is None:
                            continue
                        name = f'{basename}_{skin_index:02d}_00{suffix}.png'
                        previous = texture_files.get(name)
                        if previous and previous != texture:
                            raise ExportError(f"Texture Stem collision: {basename}; choose a unique stem for each material")
                        texture_files[name] = texture
                vertices, output_triangles, lookup = [], [], {}
                for triangle in triangles:
                    indices = []
                    points = [matrix @ mesh.vertices[i].co for i in triangle.vertices]
                    if (points[1] - points[0]).cross(points[2] - points[0]).length_squared == 0:
                        continue
                    for loop_index in triangle.loops:
                        vertex_index = mesh.loops[loop_index].vertex_index
                        uv = tuple(baked_uvs[loop_index] if baked_uvs is not None else uv_layer.data[loop_index].uv)
                        key = (vertex_index, uv)
                        if key not in lookup:
                            vertex = mesh.vertices[vertex_index]
                            position = matrix @ vertex.co
                            weights = (_weights(obj, vertex, position, inverses, groups, warnings) if rig else
                                       (Weight(0, 1.0, tuple(position)),))
                            lookup[key] = len(vertices)
                            vertices.append(Vertex((uv[0], 1.0 - uv[1]), weights))
                        indices.append(lookup[key])
                    if determinant > 0:
                        indices[1], indices[2] = indices[2], indices[1]
                    output_triangles.append(tuple(indices))
                name = f'{obj.name}:{material.name if material else material_index}'
                surfaces.extend(split_surface(Surface(name, shader, tuple(vertices), tuple(output_triangles))))
        finally:
            if evaluated:
                evaluated.to_mesh_clear()
            else:
                bpy.data.meshes.remove(mesh)
    if len(set(skin_counts)) > 1:
        raise ExportError("All surfaces must define the same number of skins; add matching variants to each material")
    return Model(joints, tuple(surfaces)), texture_files


def _solid_png(path, color):
    def srgb(value):
        value = min(1.0, max(0.0, value))
        return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055

    if len(color) != 4 or not all(math.isfinite(v) for v in color):
        raise ExportError("Material color must contain four finite values")
    rgba = bytes(round(srgb(v) * 255) for v in color[:3]) + bytes([round(min(1, max(0, color[3])) * 255)])

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 1, 1, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b'\x00' + rgba)) + chunk(b'IEND', b''))


def _save_texture(path, texture):
    if texture.path is not None:
        shutil.copyfile(texture.path, path)
        return
    if texture.image is None:
        _solid_png(path, texture.color)
        return
    source = texture.image
    # Copy pixels explicitly: Image.copy() can omit unsaved edits to the buffer.
    # Keeping the source image untouched also preserves packed/filepath state.
    pixels = array('f', [0.0]) * len(source.pixels)
    source.pixels.foreach_get(pixels)
    if texture.alpha is not None:
        if not math.isfinite(texture.alpha) or not 0 <= texture.alpha <= 1:
            raise ExportError(f"{source.name}: opacity must be finite and between zero and one")
        pixels[3::4] = array('f', [texture.alpha]) * (len(pixels) // 4)
    temporary = bpy.data.images.new('__quiver_md5_export', width=source.size[0], height=source.size[1],
                                    alpha=True, float_buffer=source.is_float)
    try:
        temporary.colorspace_settings.name = source.colorspace_settings.name
        temporary.alpha_mode = source.alpha_mode
        temporary.pixels.foreach_set(pixels)
        temporary.file_format = 'PNG'
        temporary.filepath_raw = str(path)
        temporary.save()
    finally:
        bpy.data.images.remove(temporary)
    if not path.is_file():
        raise ExportError(f"Failed to save image {source.name}")


def _previous_textures(filepath, manifest_name):
    manifest = filepath.parent / manifest_name
    if not manifest.exists():
        return set()
    try:
        if manifest.stat().st_size > 1024 * 1024:
            raise ValueError('oversized manifest')
        data = json.loads(manifest.read_text(encoding='utf-8'))
        names = data['textures']
        if (data['version'] != 1 or data['mesh'] != filepath.name or not isinstance(names, list) or
                not all(isinstance(name, str) and
                        re.fullmatch(r'[A-Za-z0-9_-]+_[0-9]{2}_00(?:_glow)?\.png', name) for name in names)):
            raise ValueError('invalid manifest')
        return set(names)
    except (KeyError, TypeError, ValueError) as exc:
        raise ExportError(f"Invalid export ownership file {manifest}; move it aside before exporting") from exc


def _check_texture_ownership(filepath, manifest_name, names):
    for other in filepath.parent.glob('.*.md5mesh.quiver.json'):
        if other.name == manifest_name:
            continue
        other_mesh = other.name[1:-len('.quiver.json')]
        overlap = names & _previous_textures(filepath.parent / other_mesh, other.name)
        if overlap:
            raise ExportError(f"Texture {min(overlap)} belongs to {other_mesh}; choose unique Texture Stems for this model")


def _publish(staging, target, names, obsolete=()):
    backups, installed = [], []
    try:
        for name in [*sorted(obsolete), *names]:
            destination = target / name
            if destination.exists():
                if not destination.is_file() or destination.is_symlink():
                    raise ExportError(f"Cannot replace non-regular file: {destination}")
                backup = staging / ('backup_' + str(len(backups)))
                os.replace(destination, backup)
                backups.append((backup, destination))
            if name not in obsolete:
                os.replace(staging / name, destination)
                installed.append(destination)
    except Exception:
        for path in reversed(installed):
            path.unlink(missing_ok=True)
        for backup, destination in reversed(backups):
            os.replace(backup, destination)
        raise


def export(context, filepath, *, selected_only=True, scale=1.0, forward_axis='NEG_Y',
           export_rig=False, apply_modifiers=True, texture_prefix='progs', copy_textures=True,
           bake_mode='AUTO', bake_size=2048, bake_margin=8, bake_samples=16,
           bake_uv_mode='AUTO', bake_size_mode='AUTO', bake_texel_density=DEFAULT_DENSITY,
           bake_min_size=64, bake_max_size=2048, force_rebake=False, bake_cache_dir=''):
    """Write .md5mesh, one-frame .md5anim, and adjacent Quake-named PNG skins.

    ``texture_prefix`` describes the eventual directory inside the game/mod;
    it never changes the disk destination selected in the file browser.
    AUTO bakes material channels that cannot be represented by direct images.
    Explicit per-material skin lists always take precedence over baking.

    ``bake_size_mode`` AUTO sizes each bake from its surface area at
    ``bake_texel_density`` pixels per game unit, between the min and max sizes;
    FIXED uses ``bake_size``. A material's Bake Size override beats both.
    Finished bakes are reused from ``bake_cache_dir`` (default: the user cache)
    unless their inputs changed or ``force_rebake`` is set.
    """
    if bake_mode not in {'AUTO', 'ALWAYS', 'OFF'}:
        raise ExportError("Material Baking must be AUTO, ALWAYS, or OFF")
    if bake_uv_mode not in {'AUTO', 'EXISTING'}:
        raise ExportError("Bake UVs must be AUTO or EXISTING")
    policy = SizePolicy(bake_size_mode, bake_size, bake_texel_density, bake_min_size, bake_max_size)
    for value, low, high, label in ((bake_margin, 0, 64, 'Bake Margin'),
                                    (bake_samples, 1, 256, 'Bake Samples')):
        if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
            raise ExportError(f"{label} must be an integer between {low} and {high}")
    warnings = set()
    with ExitStack() as resources:
        baker = None

        def open_session():
            from .baking import BakeSession
            return resources.enter_context(BakeSession(context, size=bake_size, samples=bake_samples,
                                                       margin=min(bake_margin, bake_size // 2)))

        def save_png(path, image, alpha):
            _save_texture(path, _Texture(image=image, alpha=alpha))

        def material_skins(obj, mesh, material_index, material, uv_name):
            nonlocal baker
            if material and material.quiver_md5.skins:
                if uv_name is None:
                    raise ExportError(f"{obj.name}: explicit skin images require an existing UV map")
                return _material_skins(material, uv_name), None
            if uv_name is None and (bake_mode == 'OFF' or bake_uv_mode == 'EXISTING'):
                raise ExportError(f"{obj.name}: add and unwrap a UV map or enable Automatic Bake UVs")
            if uv_name is not None and (bake_mode != 'ALWAYS' or not material or not material.use_nodes):
                try:
                    return _material_skins(material, uv_name), None
                except NeedsBaking:
                    if bake_mode == 'OFF':
                        raise
            if not copy_textures:
                raise ExportError("Enable Export PNG Skins to bake materials")
            if baker is None:
                from .bake_manager import SurfaceBaker
                root = (Path(bpy.path.abspath(bake_cache_dir)).expanduser() if bake_cache_dir
                        else bake_cache.default_directory())
                scratch = resources.enter_context(tempfile.TemporaryDirectory(prefix='quiver-md5-bake-'))
                baker = SurfaceBaker(context, open_session, bake_cache.BakeCache(root), policy, Path(scratch), save_png,
                                     margin=bake_margin, samples=bake_samples, uv_mode=bake_uv_mode,
                                     apply_modifiers=apply_modifiers, scale=scale, force=force_rebake)
            baked = baker.bake(obj, mesh, material_index, material, uv_name)
            return ((_Texture(path=baked.diffuse), _Texture(path=baked.glow) if baked.glow else None),), baked.uvs

        result = _export_bundle(context, filepath, selected_only, scale, forward_axis, export_rig,
                                apply_modifiers, texture_prefix, copy_textures, material_skins, warnings)
        if baker is None:
            return result
        return replace(result, warnings=tuple(sorted({*result.warnings, *baker.finish()})), bakes=tuple(baker.records))


def _export_bundle(context, filepath, selected_only, scale, forward_axis, export_rig,
                   apply_modifiers, texture_prefix, copy_textures, material_skins, warnings):
    if context.mode != 'OBJECT':
        raise ExportError("Switch to Object Mode before exporting")
    if not filepath:
        raise ExportError("Choose an MD5 export filename")
    filepath = Path(bpy.path.abspath(str(filepath))).expanduser().absolute().with_suffix('.md5mesh')
    if len(filepath.name.encode('utf-8')) + len(_prefix(texture_prefix).encode('ascii')) + 1 > 63:
        raise ExportError("Model game path exceeds Quake's 63-byte limit; shorten the filename or game directory")
    candidates = context.selected_objects if selected_only else context.view_layer.objects
    objects = sorted((obj for obj in candidates if obj.type == 'MESH'), key=lambda obj: obj.name_full)
    if not objects:
        raise ExportError("Select at least one mesh to export" if selected_only else "No meshes in the current view layer")
    prefix, conversion = _prefix(texture_prefix), _conversion(scale, forward_axis)
    rig = _rig_for(objects) if export_rig else None
    states = []
    try:
        if rig:
            for obj in objects:
                for modifier in obj.modifiers:
                    if modifier.type == 'ARMATURE':
                        states.append((modifier, modifier.show_viewport))
                        modifier.show_viewport = False
            context.view_layer.update()
        model, texture_files = _extract(context, objects, filepath, conversion, rig, apply_modifiers,
                                        prefix, warnings, material_skins)
        mesh_text = md5.mesh_text(model)
        anim_text = md5.bind_animation_text(model)
    finally:
        for modifier, enabled in states:
            modifier.show_viewport = enabled
        if states:
            context.view_layer.update()
    # Validate and serialize before touching existing output. Stage all image
    # conversions as well; publishing restores old files if a replacement fails.
    filepath.parent.mkdir(parents=True, exist_ok=True)
    manifest_name = f'.{filepath.name}.quiver.json'
    obsolete = _previous_textures(filepath, manifest_name) - texture_files.keys() if copy_textures else set()
    if copy_textures:
        _check_texture_ownership(filepath, manifest_name, set(texture_files) | obsolete)
    with tempfile.TemporaryDirectory(prefix='.quiver-md5-', dir=filepath.parent) as directory:
        staging = Path(directory)
        names = []
        if copy_textures:
            for name, texture in sorted(texture_files.items()):
                _save_texture(staging / name, texture)
                names.append(name)
            manifest = {'version': 1, 'mesh': filepath.name, 'textures': sorted(texture_files)}
            (staging / manifest_name).write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
            names.append(manifest_name)
        else:
            warnings.add("PNG export disabled; install matching diffuse/fullbright files for the shader paths")
        animation_name = filepath.with_suffix('.md5anim').name
        (staging / animation_name).write_text(anim_text, encoding='utf-8', newline='\n')
        (staging / filepath.name).write_text(mesh_text, encoding='utf-8', newline='\n')
        names.extend((animation_name, filepath.name))
        _publish(staging, filepath.parent, names, obsolete)
    return ExportResult(str(filepath), len(model.surfaces), sum(len(s.vertices) for s in model.surfaces),
                        sum(len(s.triangles) for s in model.surfaces), len(texture_files) if copy_textures else 0,
                        tuple(sorted(warnings)))
