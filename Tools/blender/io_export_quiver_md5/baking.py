# SPDX-License-Identifier: GPL-2.0-or-later
"""Isolated CPU Cycles baking of material colors into MD5 image skins.

Only the final shader closure is restricted. Color inputs may contain texture
mapping, procedural nodes, and nested color groups. Source data is never edited.
"""

from array import array
from dataclasses import dataclass
import math

import bpy
import bmesh

from .model import ExportError
from .geometry import clean_degenerate_faces, triangle_has_area


@dataclass(frozen=True)
class BakeResult:
    diffuse: object
    glow: object
    uvs: tuple | None = None

    def __iter__(self):
        # Preserve the original two-image unpacking API.
        yield self.diffuse
        yield self.glow


class BakeUVError(ExportError):
    """An existing map is unsuitable as the destination for a single skin."""


def _upstream(socket):
    seen = set()
    while socket.is_linked:
        link = socket.links[0]
        node = link.from_node
        if node.type != 'REROUTE':
            return node, link.from_socket
        if node.as_pointer() in seen:
            raise ExportError("Cannot bake a cyclic material connection")
        seen.add(node.as_pointer())
        socket = node.inputs[0]
    return None, None


def _matching(sockets, socket):
    return next((s for s in sockets if s.identifier == socket.identifier), None) or sockets.get(socket.name)


def _validate_inputs(sockets, material_name, *, missing_uv=False):
    """Visit reachable inputs, including nested group instances, read-only."""
    seen = set()

    def visit_input(socket, stack=()):
        for link in socket.links:
            visit_output(link.from_node, link.from_socket, stack)

    def visit_output(node, output, stack):
        key = (node.as_pointer(), output.identifier, tuple(n.as_pointer() for n in stack))
        if key in seen:
            return
        seen.add(key)
        if node.type == 'OBJECT_INFO' and output.name == 'Random':
            raise ExportError(f"{material_name}: Object Info Random depends on object identity and cannot be baked safely")
        if missing_uv and ((node.type == 'TEX_COORD' and output.name == 'UV') or node.type == 'UVMAP'):
            raise ExportError(f"{material_name}: the source material uses UV coordinates but the mesh has no source UV map")
        if node.type == 'TEX_IMAGE':
            image = node.image
            if image is None or min(image.size) <= 0 or len(image.pixels) == 0:
                raise ExportError(f"{material_name}: a reachable Image Texture is missing or has unavailable pixels; reload or pack it")
            if missing_uv and not node.inputs['Vector'].is_linked:
                raise ExportError(f"{material_name}: the source image needs its original UV map; add source UVs or use Generated/Object coordinates")
        if node.type == 'GROUP':
            if node.node_tree is None or node in stack:
                raise ExportError(f"{material_name}: invalid or recursive color node group")
            group_output = next((n for n in node.node_tree.nodes if n.type == 'GROUP_OUTPUT' and n.is_active_output), None)
            inner = _matching(group_output.inputs, output) if group_output else None
            if inner is None:
                raise ExportError(f"{material_name}: cannot resolve color node group output")
            visit_input(inner, (*stack, node))
        elif node.type == 'GROUP_INPUT':
            if stack:
                outer = _matching(stack[-1].inputs, output)
                if outer:
                    visit_input(outer, stack[:-1])
        else:
            for socket in node.inputs:
                visit_input(socket, stack)

    for socket in sockets:
        if socket is not None:
            visit_input(socket)


def _surface(material):
    outputs = [n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL'
               and n.is_active_output and n.target in {'ALL', 'CYCLES'}]
    outputs.sort(key=lambda n: n.target != 'CYCLES')
    if not outputs:
        raise ExportError(f"{material.name}: baking needs an active All or Cycles Material Output")
    node, _ = _upstream(outputs[0].inputs['Surface'])
    if node is None or node.type not in {'BSDF_PRINCIPLED', 'EMISSION'}:
        raise ExportError(f"{material.name}: baking supports a direct Principled BSDF or Emission shader; flatten Mix/Add shaders or shader groups first")
    if node.type == 'EMISSION':
        # vkQuake adds fullbright RGB to the lit diffuse. An emission-only
        # surface needs black diffuse to avoid counting the color twice.
        return None, None, node.inputs['Color'], node.inputs['Strength']
    return (node.inputs['Base Color'], node.inputs.get('Alpha'),
            node.inputs.get('Emission Color') or node.inputs.get('Emission'),
            node.inputs.get('Emission Strength'))


def _cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _overlap_measure(a, b):
    """Return intersection area and minimum thickness of two UV triangles."""
    polygon = list(a)
    sign = 1 if _cross(*b) > 0 else -1
    for edge in range(3):
        start, end = b[edge], b[(edge + 1) % 3]
        result = []
        if not polygon:
            return 0.0, 0.0
        previous = polygon[-1]
        prev_side = sign * _cross(start, end, previous)
        for point in polygon:
            side = sign * _cross(start, end, point)
            if (side >= 0) != (prev_side >= 0):
                fraction = prev_side / (prev_side - side)
                result.append(tuple(previous[k] + fraction * (point[k] - previous[k]) for k in range(2)))
            if side >= 0:
                result.append(point)
            previous, prev_side = point, side
        polygon = result
    area = abs(sum(polygon[i][0] * polygon[(i + 1) % len(polygon)][1] -
                   polygon[(i + 1) % len(polygon)][0] * polygon[i][1] for i in range(len(polygon)))) / 2
    if area <= 1e-12:
        return area, 0.0
    widths = []
    for i, point in enumerate(polygon):
        other = polygon[(i + 1) % len(polygon)]
        dx, dy = other[0] - point[0], other[1] - point[1]
        length = math.hypot(dx, dy)
        if length > 1e-15:
            distances = [(-dy * (p[0] - point[0]) + dx * (p[1] - point[1])) / length for p in polygon]
            widths.append(max(distances) - min(distances))
    return area, min(widths, default=0.0)


def _overlaps(a, b):
    # UVs are stored as float32. Collinear/shared edges can acquire an almost
    # zero-width intersection on roundoff; area alone misclassifies long slivers.
    area, thickness = _overlap_measure(a, b)
    return area > 1e-12 and thickness > 2e-7


def _validate_uv(mesh, material_index, uv_name, object_name):
    layer = mesh.uv_layers.get(uv_name) if uv_name else None
    if layer is None:
        raise BakeUVError(f"{object_name}: baking needs a destination UV map")
    mesh.calc_loop_triangles()
    triangles, face_indices = [], []
    for triangle in mesh.loop_triangles:
        if triangle.material_index != material_index or not triangle_has_area(mesh, triangle):
            continue
        coords = tuple(tuple(layer.data[i].uv) for i in triangle.loops)
        if any(not math.isfinite(v) or v < -1e-6 or v > 1.000001 for uv in coords for v in uv):
            raise BakeUVError(f"{object_name}: baking requires UVs inside the 0–1 tile; unwrap the export UV map first")
        if abs(_cross(*coords)) <= 1e-12:
            raise BakeUVError(f"{object_name}: baking requires UV faces with nonzero area")
        triangles.append(coords)
        face_indices.append(triangle.polygon_index)
    if not triangles:
        raise ExportError(f"{object_name}: no faces to bake for material slot {material_index}")
    cells = max(1, min(128, int(math.sqrt(len(triangles)))))
    buckets = {}
    for index, triangle in enumerate(triangles):
        low = [max(0, min(cells - 1, int(min(p[axis] for p in triangle) * cells))) for axis in range(2)]
        high = [max(0, min(cells - 1, int(max(p[axis] for p in triangle) * cells))) for axis in range(2)]
        candidates = set()
        keys = [(x, y) for x in range(low[0], high[0] + 1) for y in range(low[1], high[1] + 1)]
        for key in keys:
            candidates.update(buckets.get(key, ()))
        for other in candidates:
            if _overlaps(triangle, triangles[other]):
                area, _ = _overlap_measure(triangle, triangles[other])
                raise BakeUVError(f"{object_name}: UV faces {face_indices[other]} and {face_indices[index]} overlap "
                                  f"(area {area:.3g}); use unique bake UVs")
        for key in keys:
            buckets.setdefault(key, []).append(index)


class BakeSession:
    """Own temporary render data and baked images until the context exits."""

    def __init__(self, context, size=1024, margin=8, samples=16):
        if not isinstance(size, int) or not 16 <= size <= 8192:
            raise ExportError("Bake resolution must be an integer from 16 to 8192")
        if not isinstance(margin, int) or not 0 <= margin <= size // 2:
            raise ExportError("Bake margin must be between zero and half the image resolution")
        if not isinstance(samples, int) or not 1 <= samples <= 4096:
            raise ExportError("Bake samples must be an integer from 1 to 4096")
        self.context, self.size, self.margin, self.samples = context, size, margin, samples
        self.scene = None
        self.images, self.objects, self.meshes, self.materials = [], [], [], []

    def __enter__(self):
        if self.scene is not None:
            raise ExportError("A bake session cannot be entered twice")
        try:
            self.scene = bpy.data.scenes.new('__quiver_md5_bake')
            self.scene.render.engine = 'CYCLES'
            self.scene.cycles.device = 'CPU'
            self.scene.cycles.samples = self.samples
            self.scene.cycles.use_denoising = False
            self.scene.frame_set(self.context.scene.frame_current, subframe=self.context.scene.frame_subframe)
            self.scene.render.use_simplify = self.context.scene.render.use_simplify
            self.scene.render.simplify_subdivision_render = self.context.scene.render.simplify_subdivision
            return self
        except Exception as exc:
            self.close()
            raise ExportError("CPU Cycles baking is unavailable in this Blender build; enable Cycles or export pre-baked image skins") from exc

    def __exit__(self, exc_type, exc, traceback):
        self.close()

    def close(self):
        self._release((0, 0, 0))
        for image in reversed(self.images):
            bpy.data.images.remove(image, do_unlink=True)
        self.images.clear()
        if self.scene is not None:
            bpy.data.scenes.remove(self.scene, do_unlink=True)
            self.scene = None

    def _release(self, marker):
        for collection, owned, start in ((bpy.data.objects, self.objects, marker[0]),
                                         (bpy.data.meshes, self.meshes, marker[1]),
                                         (bpy.data.materials, self.materials, marker[2])):
            for data in reversed(owned[start:]):
                collection.remove(data, do_unlink=True)
            del owned[start:]

    def _image(self, name, *, scalar=False):
        image = bpy.data.images.new('__quiver_md5_' + name, width=self.size, height=self.size, alpha=True)
        self.images.append(image)
        image.colorspace_settings.name = 'Non-Color' if scalar else 'sRGB'
        image.generated_color = (0, 0, 0, 1)
        return image

    def _material(self, name):
        material = bpy.data.materials.new('__quiver_md5_' + name)
        self.materials.append(material)
        material.use_nodes = True
        return material

    def _prepare(self, obj, mesh, material_index, uv_name, apply_modifiers, generate_uv=False):
        source = (obj.material_slots[material_index].material if material_index < len(obj.material_slots)
                  else mesh.materials[material_index] if material_index < len(mesh.materials) else None)
        temporary = obj.copy()
        self.objects.append(temporary)
        data = (obj.data if apply_modifiers else mesh).copy()
        self.meshes.append(data)
        clean_degenerate_faces(data)
        temporary.data = data
        temporary.animation_data_clear()
        temporary.parent = None
        temporary.constraints.clear()
        if apply_modifiers:
            for modifier in temporary.modifiers:
                modifier.show_render = modifier.show_viewport
                if modifier.type in {'SUBSURF', 'MULTIRES'}:
                    modifier.render_levels = modifier.levels
        else:
            temporary.modifiers.clear()
            if data.shape_keys:
                temporary.shape_key_clear()
        self.scene.collection.objects.link(temporary)
        temporary.matrix_world = obj.matrix_world.copy()
        temporary.hide_viewport = temporary.hide_render = False
        temporary.hide_set(False, view_layer=self.scene.view_layers[0])
        if not generate_uv and (not uv_name or uv_name not in data.uv_layers):
            raise ExportError(f"{obj.name}: the chosen bake UV must exist before modifiers; apply UV-generating modifiers first")
        # uv_layer= below selects the bake destination. Leave active_render alone:
        # default upstream image/UV nodes must continue sampling their source map.
        target = source.copy() if source else self._material('surface')
        if source:
            self.materials.append(target)
        if not source or not source.use_nodes:
            target.use_nodes = True
            shader = next(n for n in target.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
            color = tuple(source.diffuse_color) if source else (0.8, 0.8, 0.8, 1.0)
            shader.inputs['Base Color'].default_value = color
            shader.inputs['Alpha'].default_value = color[3]
        scratch = self._material('scratch')
        image = self._image('scratch')
        scratch_node = scratch.node_tree.nodes.new('ShaderNodeTexImage')
        scratch_node.image = image
        scratch.node_tree.nodes.active = scratch_node
        # Clearing slots clamps polygon material indices to zero in Blender.
        # Keep assignments while replacing only the temporary slot materials.
        material_indices = tuple(polygon.material_index for polygon in data.polygons)
        data.materials.clear()
        count = max(material_index + 1, len(mesh.materials), len(obj.material_slots))
        for index in range(count):
            data.materials.append(target if index == material_index else scratch)
        for polygon, index in zip(data.polygons, material_indices):
            polygon.material_index = index
        for slot in temporary.material_slots:
            slot.link = 'DATA'
        temporary.select_set(True, view_layer=self.scene.view_layers[0])
        self.scene.view_layers[0].objects.active = temporary
        self.scene.view_layers[0].update()
        with self.context.temp_override(scene=self.scene, view_layer=self.scene.view_layers[0]):
            evaluated = temporary.evaluated_get(self.context.evaluated_depsgraph_get())
            evaluated_materials = [mat.original if mat else None for mat in evaluated.data.materials]
            if any(mat not in (target, scratch) for mat in evaluated_materials):
                raise ExportError(f"{obj.name}: a modifier assigns external materials; apply material-generating modifiers before baking")
            if (material_index >= len(evaluated_materials) or
                    evaluated_materials[material_index] != target or
                    not any(poly.material_index == material_index for poly in evaluated.data.polygons)):
                raise ExportError(f"{obj.name}: modifiers changed bake material slots; apply those modifiers before baking")
        return temporary, target, image

    def _smart_project(self, temporary, mesh, material_index, object_name):
        """Add a destination map without changing any source material mapping.

        Projection runs on the copied base mesh. Existing modifier interpolation
        carries the new map to final geometry, which must match the export mesh.
        """
        data = temporary.data
        # Never retain UV layer RNA references across collection edits or mode
        # changes: Blender can reallocate those structs.
        render_name = next((layer.name for layer in data.uv_layers if layer.active_render), None)
        if render_name is None and data.uv_layers.active is not None:
            render_name = data.uv_layers.active.name
        layer = data.uv_layers.new(name='__quiver_md5_export_uv', do_init=False)
        if layer is None:
            raise ExportError(f"{object_name}: cannot add an automatic export UV map; remove an unused UV map first")
        name = layer.name
        del layer
        data.uv_layers.active_index = len(data.uv_layers) - 1
        if render_name:
            data.uv_layers.get(render_name).active_render = True
        self._edit_projection(temporary, material_index, self._project_primary)
        if render_name:
            data.uv_layers.get(render_name).active_render = True
        try:
            return name, self._generated_uvs(temporary, mesh, material_index, name, object_name)
        except BakeUVError:
            # Dense/folded geometry can leave real overlaps in Smart Project.
            # Try individual-face lightmap packing, then a disjoint-cell atlas.
            self._fallback_uv(temporary, material_index, name)
        try:
            return name, self._generated_uvs(temporary, mesh, material_index, name, object_name)
        except BakeUVError:
            self._grid_fallback_uv(temporary, material_index, name)
        try:
            return name, self._generated_uvs(temporary, mesh, material_index, name, object_name)
        except BakeUVError as exc:
            remedy = ("apply enabled modifiers or use an existing unwrapped UV map"
                      if any(modifier.show_viewport for modifier in temporary.modifiers)
                      else "use an existing unwrapped UV map or a cleaned mesh copy")
            raise ExportError(f"{object_name}: automatic UV packing could not produce a unique bake layout; {remedy} ({exc})") from exc

    def _edit_projection(self, temporary, material_index, projector):
        data = temporary.data
        render_name = next((layer.name for layer in data.uv_layers if layer.active_render), None)
        view_layer = self.scene.view_layers[0]
        with self.context.temp_override(scene=self.scene, view_layer=view_layer, object=temporary,
                                        active_object=temporary, selected_objects=[temporary],
                                        selected_editable_objects=[temporary]):
            try:
                bpy.ops.object.mode_set(mode='EDIT')
                # The window remains in the source scene; edit-mesh operators
                # need the temporary edit object supplied explicitly.
                with self.context.temp_override(edit_object=temporary, objects_in_mode=[temporary],
                                                objects_in_mode_unique_data=[temporary], mode='EDIT_MESH'):
                    self.scene.tool_settings.mesh_select_mode = (False, False, True)
                    bpy.ops.mesh.reveal(select=True)
                    bpy.ops.mesh.select_all(action='DESELECT')
                    editable = bmesh.from_edit_mesh(data)
                    for face in editable.faces:
                        if face.material_index == material_index:
                            face.select_set(True)
                    bmesh.update_edit_mesh(data, loop_triangles=False, destructive=False)
                    projector()
            finally:
                if temporary.mode != 'OBJECT':
                    bpy.ops.object.mode_set(mode='OBJECT')
                if render_name:
                    data.uv_layers.get(render_name).active_render = True
        view_layer.update()

    def _project_primary(self):
        """Primary projection hook; called inside the isolated edit context."""
        bpy.ops.uv.smart_project(angle_limit=math.radians(66),
                                 island_margin=min(0.1, (2 * self.margin + 1) / self.size),
                                 area_weight=0, correct_aspect=False, scale_to_bounds=True)

    def _fallback_uv(self, temporary, material_index, uv_name):
        """Pack only the requested material's faces, preserving source UVs."""
        def project():
            bpy.ops.uv.lightmap_pack(PREF_CONTEXT='SEL_FACES', PREF_PACK_IN_ONE=True,
                                     PREF_NEW_UVLAYER=False, PREF_BOX_DIV=12,
                                     PREF_MARGIN_DIV=max(0.001, self.margin / self.size * 100))
        self._edit_projection(temporary, material_index, project)

    def _grid_fallback_uv(self, temporary, material_index, uv_name):
        """Last resort: convex face islands in provably disjoint grid cells.

        Source positions/topology and source UV layers stay intact. This may
        introduce more seams and stretch than the primary atlas, but cannot
        stack different polygons in the same destination cell.
        """
        data = temporary.data
        faces = [face for face in data.polygons if face.material_index == material_index]
        if not faces:
            raise ExportError(f"{temporary.name}: no faces remain for material {material_index}")
        columns = math.ceil(math.sqrt(len(faces)))
        rows = math.ceil(len(faces) / columns)
        width, height = 1.0 / columns, 1.0 / rows
        pad_x = min((self.margin + 1) / self.size, width * 0.125)
        pad_y = min((self.margin + 1) / self.size, height * 0.125)
        layer = data.uv_layers.get(uv_name)
        for index, face in enumerate(faces):
            if face.loop_total == 3:
                coords = ((0, 0), (1, 0), (0.5, 1))
            elif face.loop_total == 4:
                coords = ((0, 0), (1, 0), (1, 1), (0, 1))
            else:
                coords = tuple((math.cos(2 * math.pi * i / face.loop_total),
                                math.sin(2 * math.pi * i / face.loop_total)) for i in range(face.loop_total))
                low = [min(point[k] for point in coords) for k in range(2)]
                span = [max(point[k] for point in coords) - low[k] for k in range(2)]
                coords = tuple(tuple((point[k] - low[k]) / span[k] for k in range(2)) for point in coords)
            column, row = index % columns, index // columns
            for loop, coord in zip(face.loop_indices, coords):
                layer.data[loop].uv = (column * width + pad_x + coord[0] * (width - 2 * pad_x),
                                       row * height + pad_y + coord[1] * (height - 2 * pad_y))
        self.scene.view_layers[0].update()

    def _generated_uvs(self, temporary, mesh, material_index, name, object_name):
        view_layer = self.scene.view_layers[0]
        view_layer.update()
        with self.context.temp_override(scene=self.scene, view_layer=view_layer):
            evaluated = temporary.evaluated_get(self.context.evaluated_depsgraph_get())
            generated = evaluated.data
            if any(modifier.show_viewport for modifier in temporary.modifiers):
                # The reference export mesh is cleaned after evaluation too.
                generated = generated.copy()
                self.meshes.append(generated)
                clean_degenerate_faces(generated)
            remedy = ("apply enabled modifiers or use an existing unwrapped UV map"
                      if any(modifier.show_viewport for modifier in temporary.modifiers)
                      else "use an existing unwrapped UV map or a cleaned mesh copy")
            if (len(generated.vertices) != len(mesh.vertices) or len(generated.loops) != len(mesh.loops) or
                    len(generated.polygons) != len(mesh.polygons) or
                    any(a.vertex_index != b.vertex_index for a, b in zip(generated.loops, mesh.loops)) or
                    any((a.loop_start, a.loop_total, a.material_index) != (b.loop_start, b.loop_total, b.material_index)
                        for a, b in zip(generated.polygons, mesh.polygons))):
                raise ExportError(f"{object_name}: automatic bake UVs no longer match the exported face layout; {remedy}")
            extent = max((v.co.length for v in mesh.vertices), default=1)
            tolerance = max(1e-5, extent * 1e-5)
            if any((a.co - b.co).length > tolerance for a, b in zip(generated.vertices, mesh.vertices)):
                raise ExportError(f"{object_name}: automatic bake UVs no longer match the exported vertex positions; {remedy}")
            _validate_uv(generated, material_index, name, object_name)
            # Modifier interpolation can leave roundoff just outside the tile.
            # Validation above rejects meaningful excursions before clamping.
            uvs = tuple(tuple(min(1.0, max(0.0, value)) for value in corner.uv)
                        for corner in generated.uv_layers.get(name).data)
        return uvs

    @staticmethod
    def _connect(tree, source, destination):
        if source.is_linked:
            tree.links.new(source.links[0].from_socket, destination)
        elif destination.type == 'RGBA' and source.type != 'RGBA':
            value = float(source.default_value)
            destination.default_value = (value, value, value, 1)
        else:
            destination.default_value = source.default_value

    def _bake_image(self, scene, obj, material, color_socket, strength_socket=None, label='Diffuse'):
        """One emission pass; a small explicit seam for failure-injection tests."""
        image = self._image(label, scalar=label == 'Alpha')
        tree = material.node_tree
        for node in list(tree.nodes):
            if node.type == 'OUTPUT_MATERIAL':
                tree.nodes.remove(node)
        output = tree.nodes.new('ShaderNodeOutputMaterial')
        output.target = 'CYCLES'
        output.is_active_output = True
        emitter = tree.nodes.new('ShaderNodeEmission')
        self._connect(tree, color_socket, emitter.inputs['Color'])
        if strength_socket is not None:
            self._connect(tree, strength_socket, emitter.inputs['Strength'])
        else:
            emitter.inputs['Strength'].default_value = 1
        tree.links.new(emitter.outputs[0], output.inputs['Surface'])
        texture = tree.nodes.new('ShaderNodeTexImage')
        texture.image = image
        tree.nodes.active = texture
        view_layer = scene.view_layers[0]
        with self.context.temp_override(scene=scene, view_layer=view_layer, object=obj, active_object=obj,
                                        selected_objects=[obj], selected_editable_objects=[obj]):
            result = bpy.ops.object.bake(type='EMIT', target='IMAGE_TEXTURES', save_mode='INTERNAL',
                                         use_selected_to_active=False, use_clear=True, margin=self.margin,
                                         margin_type='EXTEND', uv_layer=self._uv_name)
        if 'FINISHED' not in result:
            raise ExportError("CPU Cycles did not finish the material bake")
        return image

    @staticmethod
    def _pixels(image):
        pixels = array('f', [0.0]) * len(image.pixels)
        image.pixels.foreach_get(pixels)
        return pixels

    def bake(self, obj, mesh, material_index, uv_name, *, apply_modifiers=True, uv_mode='AUTO'):
        if self.scene is None:
            raise ExportError("Use BakeSession as a context manager")
        if uv_mode not in {'AUTO', 'EXISTING'}:
            raise ExportError("Bake UV mode must be AUTO or EXISTING")
        generate_uv = False
        try:
            _validate_uv(mesh, material_index, uv_name, obj.name)
        except BakeUVError:
            if uv_mode == 'EXISTING':
                raise
            generate_uv = True
        marker = (len(self.objects), len(self.meshes), len(self.materials))
        scratch = None
        try:
            temporary, material, scratch = self._prepare(obj, mesh, material_index, uv_name, apply_modifiers, generate_uv)
            base, alpha, emission, strength = _surface(material)
            _validate_inputs((base, alpha, emission, strength), material.name, missing_uv=not mesh.uv_layers)
            self._uv_name, uvs = (self._smart_project(temporary, mesh, material_index, obj.name) if generate_uv
                                  else (uv_name, None))
            diffuse = (self._bake_image(self.scene, temporary, material, base) if base is not None
                       else self._image('Diffuse'))
            pixels = self._pixels(diffuse)
            if alpha is not None and alpha.is_linked:
                alpha_image = self._bake_image(self.scene, temporary, material, alpha, label='Alpha')
                opacity = self._pixels(alpha_image)
                for i in range(3, len(pixels), 4):
                    pixels[i] = min(1.0, max(0.0, opacity[i - 3]))
                self.images.remove(alpha_image)
                bpy.data.images.remove(alpha_image)
            else:
                opacity = min(1.0, max(0.0, float(alpha.default_value))) if alpha else 1.0
                for i in range(3, len(pixels), 4):
                    pixels[i] = opacity
            diffuse.pixels.foreach_set(pixels)
            glow = None
            has_emission = (emission is not None and (emission.is_linked or any(v > 0 for v in emission.default_value[:3])) and
                            (strength is None or strength.is_linked or strength.default_value > 0))
            if has_emission:
                glow = self._bake_image(self.scene, temporary, material, emission, strength, label='Fullbright')
                pixels = self._pixels(glow)
                if not any(pixels[i] > 1e-7 for i in range(len(pixels)) if i % 4 != 3):
                    self.images.remove(glow)
                    bpy.data.images.remove(glow)
                    glow = None
                else:
                    for i in range(3, len(pixels), 4):
                        pixels[i] = 1.0
                    glow.pixels.foreach_set(pixels)
            return BakeResult(diffuse, glow, uvs)
        except (RuntimeError, TypeError) as exc:
            raise ExportError(f"{obj.name}: CPU Cycles baking failed: {exc}") from exc
        finally:
            self._release(marker)
            if scratch is not None:
                self.images.remove(scratch)
                bpy.data.images.remove(scratch)
