# SPDX-License-Identifier: GPL-2.0-or-later
"""Digest the Blender data a bake reads, so unchanged surfaces can be reused.

A false match would reuse stale pixels, while a false mismatch only costs a bake,
so every shortcut is opt-in. Only nodes known to read nothing but their inputs
let a key ignore the rest of the mesh or the object's transform; unrecognised
nodes bind the key to both. Data that cannot be digested marks the key volatile,
which keeps it out of the disk cache.

Layout and cosmetic state are left out: only nodes reachable from the baked
channels count, and names, positions, and selection do not.
"""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path

import bpy
import numpy as np

# Their output depends only on their inputs and properties.
_PURE_NODES = frozenset({
    'NodeGroupInput', 'NodeGroupOutput', 'NodeReroute', 'ShaderNodeBlackbody', 'ShaderNodeBrightContrast',
    'ShaderNodeBsdfPrincipled', 'ShaderNodeClamp', 'ShaderNodeCombineColor', 'ShaderNodeCombineXYZ',
    'ShaderNodeEmission', 'ShaderNodeFloatCurve', 'ShaderNodeGamma', 'ShaderNodeGroup',
    'ShaderNodeHueSaturation', 'ShaderNodeInvert', 'ShaderNodeMapRange', 'ShaderNodeMapping', 'ShaderNodeMath',
    'ShaderNodeMix', 'ShaderNodeMixRGB', 'ShaderNodeNormal', 'ShaderNodeRGB', 'ShaderNodeRGBCurve',
    'ShaderNodeRGBToBW', 'ShaderNodeSeparateColor', 'ShaderNodeSeparateXYZ', 'ShaderNodeUVMap',
    'ShaderNodeValToRGB', 'ShaderNodeValue', 'ShaderNodeVectorCurve', 'ShaderNodeVectorMath',
    'ShaderNodeVectorRotate', 'ShaderNodeVertexColor', 'ShaderNodeWavelength',
})
# Pure too, but an unlinked Vector input falls back to Generated coordinates,
# which follow the bounds of the whole mesh.
_PROCEDURAL_NODES = frozenset({
    'ShaderNodeTexBrick', 'ShaderNodeTexChecker', 'ShaderNodeTexGabor', 'ShaderNodeTexGradient',
    'ShaderNodeTexMagic', 'ShaderNodeTexNoise', 'ShaderNodeTexVoronoi', 'ShaderNodeTexWave',
    'ShaderNodeTexWhiteNoise',
})
_NODE_SKIP = frozenset({
    'rna_type', 'name', 'label', 'location', 'location_absolute', 'width', 'height', 'dimensions',
    'inputs', 'outputs', 'panel_states', 'internal_links', 'parent', 'warning_propagation',
    'use_custom_color', 'color', 'color_tag', 'select', 'show_options', 'show_preview', 'hide',
    'show_texture', 'is_active_output',
})
_STRUCT_SKIP = frozenset({'rna_type', 'select', 'frame_current'})
_MODIFIER_SKIP = frozenset({'rna_type', 'name', 'execution_time', 'use_pin_to_last'})
# Selection and visibility attributes, plus indices that the topology hash
# replaces with surface-relative ones.
_ATTRIBUTE_PREFIXES = ('.select', '.hide', '.uv_select', '.vs.', '.es.')
_ATTRIBUTE_SKIP = frozenset({'.corner_vert', '.corner_edge', 'material_index'})
_ATTRIBUTE_LAYOUT = {
    'FLOAT': ('value', np.float32, 1), 'INT': ('value', np.int32, 1), 'INT8': ('value', np.int32, 1),
    'BOOLEAN': ('value', np.bool_, 1), 'FLOAT_VECTOR': ('vector', np.float32, 3),
    'FLOAT2': ('vector', np.float32, 2), 'FLOAT_COLOR': ('color', np.float32, 4),
    'BYTE_COLOR': ('color', np.float32, 4), 'INT16_2D': ('value', np.int32, 2),
    'INT32_2D': ('value', np.int32, 2), 'QUATERNION': ('value', np.float32, 4),
    'FLOAT4X4': ('value', np.float32, 16),
}


def _hex(*chunks):
    digest = hashlib.blake2b(digest_size=32)
    for chunk in chunks:
        digest.update(chunk if isinstance(chunk, (bytes, memoryview)) else np.ascontiguousarray(chunk).data)
    return digest.hexdigest()


def _text_hex(value):
    return _hex(json.dumps(value, sort_keys=True, separators=(',', ':')).encode('utf-8'))


def _plain(value):
    """Convert an RNA value to nested JSON-compatible lists and scalars."""
    if isinstance(value, (bool, int, float, str)) or value is None:
        return value
    if isinstance(value, (set, frozenset)):
        return sorted(value)
    try:
        return [_plain(item) for item in value]
    except TypeError:
        return repr(value)


def _read(collection, name, dtype, width=1):
    values = np.empty(len(collection) * width, dtype=dtype)
    collection.foreach_get(name, values)
    return values


def surface_corners(mesh, material_index=None):
    """Return face mask and corner indices for one slot, or for all faces."""
    starts = _read(mesh.polygons, 'loop_start', np.int32)
    totals = _read(mesh.polygons, 'loop_total', np.int32)
    faces = (np.ones(len(starts), dtype=np.bool_) if material_index is None
             else _read(mesh.polygons, 'material_index', np.int32) == material_index)
    starts, totals = starts[faces], totals[faces]
    offsets = np.cumsum(totals) - totals
    corners = np.repeat(starts - offsets, totals) + np.arange(int(totals.sum()), dtype=np.int64)
    return faces, corners


def mesh_digest(mesh, material_index=None):
    """Digest one material slot's faces, or the whole mesh for ``None``.

    Surface-relative vertex numbering makes the result independent of how many
    other faces surround the surface, so an edit elsewhere leaves it unchanged.
    """
    faces, corners = surface_corners(mesh, material_index)
    vertices = _read(mesh.loops, 'vertex_index', np.int32)[corners]
    used, topology = np.unique(vertices, return_inverse=True)
    totals = _read(mesh.polygons, 'loop_total', np.int32)[faces]
    parts = [_hex(totals, topology.astype(np.int64))]
    for attribute in sorted(mesh.attributes, key=lambda item: item.name):
        name = attribute.name
        if name in _ATTRIBUTE_SKIP or name.startswith(_ATTRIBUTE_PREFIXES):
            continue
        indices = {'POINT': used, 'CORNER': corners, 'FACE': faces}.get(attribute.domain)
        if indices is None and material_index is not None:
            continue  # Edge data only reaches a surface through normals.
        header = [name, attribute.domain, attribute.data_type]
        if attribute.data_type == 'STRING':
            values = [item.value for item in attribute.data]
            selected = values if indices is None else np.array(values, dtype=object)[indices].tolist()
            parts.append(_text_hex([header, selected]))
            continue
        layout = _ATTRIBUTE_LAYOUT.get(attribute.data_type)
        if layout is None:
            parts.append(_text_hex([header, 'unknown', len(attribute.data)]))
            continue
        field, dtype, width = layout
        values = _read(attribute.data, field, dtype, width).reshape(-1, width)
        parts.append(_text_hex(header) + _hex(values if indices is None else values[indices]))
    return _text_hex(parts)


def _surface_triangles(mesh, material_index):
    mesh.calc_loop_triangles()
    triangles = mesh.loop_triangles
    selected = _read(triangles, 'material_index', np.int32) == material_index
    return triangles, selected


def world_area(mesh, material_index, matrix):
    """Total area of one slot's triangles under ``matrix``."""
    triangles, selected = _surface_triangles(mesh, material_index)
    indices = _read(triangles, 'vertices', np.int32, 3).reshape(-1, 3)[selected]
    positions = _read(mesh.vertices, 'co', np.float64, 3).reshape(-1, 3)
    points = positions[indices] @ np.array(matrix.to_3x3(), dtype=np.float64).T
    return float(np.linalg.norm(np.cross(points[:, 1] - points[:, 0], points[:, 2] - points[:, 0]), axis=1).sum() / 2)


def uv_coverage(mesh, material_index, uvs):
    """Fraction of the tile one slot covers; ``uvs`` holds every corner of the mesh."""
    triangles, selected = _surface_triangles(mesh, material_index)
    corners = _read(triangles, 'loops', np.int32, 3).reshape(-1, 3)[selected]
    a, b, c = (uvs[corners[:, index]] for index in range(3))
    first, second = b - a, c - a
    return float(np.abs(first[:, 0] * second[:, 1] - first[:, 1] * second[:, 0]).sum() / 2)


def layer_coverage(mesh, material_index, uv_name):
    return uv_coverage(mesh, material_index, _read(mesh.uv_layers[uv_name].data, 'uv', np.float64, 2).reshape(-1, 2))


def tuple_coverage(mesh, material_index, uvs):
    return uv_coverage(mesh, material_index, np.array(uvs, dtype=np.float64).reshape(-1, 2))


def _custom_properties(holder):
    """Read ID properties without the memory addresses their repr can contain."""
    try:
        keys = holder.keys()
    except TypeError:  # Most modifier types have no ID properties.
        return {}
    values = {key: holder[key] for key in keys}
    return {key: repr(value.to_dict() if hasattr(value, 'to_dict') else
                      value.to_list() if hasattr(value, 'to_list') else value)
            for key, value in values.items()}


def object_digest(obj):
    """State an Object Info, Attribute, or world-space node could read."""
    return _text_hex([_plain(obj.matrix_world), _plain(obj.color), obj.pass_index,
                      _custom_properties(obj), _custom_properties(obj.data)])


def modifier_digest(obj):
    records = []
    for modifier in obj.modifiers:
        values = {}
        for prop in modifier.bl_rna.properties:
            name = prop.identifier
            if name in _MODIFIER_SKIP or name.startswith('is_') or (name.startswith('show_') and name != 'show_viewport'):
                continue
            value = getattr(modifier, name, None)
            if prop.type == 'POINTER':
                value = value.name_full if isinstance(value, bpy.types.ID) else None
            elif prop.type == 'COLLECTION':
                value = len(value)
            values[name] = _plain(value)
        records.append([modifier.type, values, _custom_properties(modifier)])  # Geometry Nodes inputs.
    return _text_hex(records)


def code_digest():
    """Changing how bakes are produced or keyed retires every earlier entry."""
    here = Path(__file__).resolve().parent
    return _hex(*((here / name).read_bytes() for name in ('baking.py', 'bake_inputs.py')))


@dataclass(frozen=True)
class MaterialDigest:
    digest: str
    whole_mesh: bool = False  # Reads data outside the slot's own faces.
    object_bound: bool = False  # Reads the transform or other object state.
    animated: bool = False
    volatile: bool = False  # Has inputs a digest cannot represent.


class MaterialHasher:
    """Digest materials, reusing image and group results across one export."""

    def __init__(self):
        self._images, self._trees = {}, {}
        self._reset()

    def _reset(self):
        self.whole_mesh = self.object_bound = self.animated = self.volatile = False

    def _flags_for(self, node, output):
        kind = node.bl_idname
        if kind in _PURE_NODES:
            return
        if kind in _PROCEDURAL_NODES:
            self.whole_mesh |= not node.inputs['Vector'].is_linked
        elif kind == 'ShaderNodeTexImage':
            self.whole_mesh |= node.projection != 'FLAT'
        elif kind == 'ShaderNodeTexCoord' and output.name in {'UV', 'Generated', 'Object'}:
            self.whole_mesh |= output.name == 'Generated'
            self.object_bound |= output.name == 'Object' and node.object is not None
        elif kind == 'ShaderNodeAttribute' and node.attribute_type == 'GEOMETRY':
            pass
        else:
            self.whole_mesh = self.object_bound = True

    def _image(self, image):
        if image.source not in {'FILE', 'GENERATED'}:
            # Sequences, movies, and UDIMs have pixels that image.pixels omits.
            self.volatile = True
            return ['volatile', image.source]
        key = image.as_pointer()
        if key not in self._images:
            pixels = np.empty(len(image.pixels), dtype=np.float32)
            image.pixels.foreach_get(pixels)
            self._images[key] = [list(image.size), image.colorspace_settings.name, image.alpha_mode, _hex(pixels)]
        return self._images[key]

    def _id(self, value):
        if isinstance(value, bpy.types.Image):
            return ['image', self._image(value)]
        if isinstance(value, bpy.types.NodeTree):
            return ['tree', self._tree(value)]
        if isinstance(value, bpy.types.Text):
            return ['text', _text_hex(value.as_string())]
        if isinstance(value, bpy.types.Object):
            self.object_bound = True
            return ['object', _plain(value.matrix_world)]
        self.volatile = True
        return ['id', type(value).__name__]

    def _struct(self, owner, skip=_STRUCT_SKIP, depth=0):
        if depth > 8:
            self.volatile = True
            return None
        values = {}
        for prop in owner.bl_rna.properties:
            name = prop.identifier
            if name in skip or name.startswith('bl_'):
                continue
            value = getattr(owner, name, None)
            if prop.type == 'POINTER':
                if isinstance(value, bpy.types.ID):
                    value = self._id(value)
                elif value is not None:
                    value = self._struct(value, depth=depth + 1)
            elif prop.type == 'COLLECTION':
                value = [self._struct(item, depth=depth + 1) for item in value]
            else:
                if prop.type == 'STRING' and getattr(prop, 'subtype', '') == 'FILE_PATH' and value:
                    values[name + ':stat'] = self._file_state(value)
                value = _plain(value)
            values[name] = value
        return values

    @staticmethod
    def _file_state(path):
        try:
            status = Path(bpy.path.abspath(path)).stat()
        except OSError:
            return None
        return [status.st_size, status.st_mtime_ns]

    def _graph(self, sockets):
        ids, records = {}, []

        def visit_socket(socket):
            if socket is None:
                return None
            if not socket.is_linked:
                return ['value', _plain(getattr(socket, 'default_value', None))]
            link = socket.links[0]
            muted, seen = link.is_muted, set()
            while link.from_node.bl_idname == 'NodeReroute':
                upstream = link.from_node.inputs[0]
                if not upstream.is_linked or link.from_node.as_pointer() in seen:
                    return ['none']
                seen.add(link.from_node.as_pointer())
                link = upstream.links[0]
                muted |= link.is_muted
            self._flags_for(link.from_node, link.from_socket)
            return ['link', visit_node(link.from_node), link.from_socket.identifier, muted]

        def visit_node(node):
            key = node.as_pointer()
            if key not in ids:
                ids[key] = len(ids)
                properties = self._struct(node, _NODE_SKIP)
                inputs = [[socket.identifier, visit_socket(socket)] for socket in node.inputs]
                records.append([ids[key], node.bl_idname, properties, inputs])
            return ids[key]

        return [[visit_socket(socket) for socket in sockets], records]

    def _tree(self, tree):
        key = tree.as_pointer()
        if key not in self._trees:
            # Measure this group alone: its flags must not depend on which
            # material happened to reach it first.
            outer = (self.whole_mesh, self.object_bound, self.animated, self.volatile)
            self._reset()
            self._trees[key] = None  # Validation rejects recursive groups.
            self.animated = tree.animation_data is not None
            output = next((node for node in tree.nodes if node.type == 'GROUP_OUTPUT' and node.is_active_output), None)
            digest = _text_hex(self._graph(output.inputs if output else ()))
            self._trees[key] = (digest, self.whole_mesh, self.object_bound, self.animated, self.volatile)
            self.whole_mesh, self.object_bound, self.animated, self.volatile = outer
        cached = self._trees[key]
        if cached is None:
            return None
        self.whole_mesh |= cached[1]
        self.object_bound |= cached[2]
        self.animated |= cached[3]
        self.volatile |= cached[4]
        return cached[0]

    def material(self, material, sockets=None):
        """Digest a material's baked channels; ``sockets`` is ``None`` without nodes."""
        self._reset()
        if material is None:
            return MaterialDigest(_text_hex(['default']))
        settings = [material.pass_index]
        if sockets is None:
            return MaterialDigest(_text_hex(['flat', _plain(material.diffuse_color), settings]))
        self.animated = material.animation_data is not None or material.node_tree.animation_data is not None
        graph = self._graph(sockets)
        return MaterialDigest(_text_hex(['nodes', graph, settings]), self.whole_mesh,
                              self.object_bound, self.animated, self.volatile)
