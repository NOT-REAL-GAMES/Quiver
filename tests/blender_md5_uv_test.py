# SPDX-License-Identifier: GPL-2.0-or-later
"""Generated export bake UV checks, including source-coordinate preservation.

    blender --background --factory-startup --python-exit-code 1 \
        --python tests/blender_md5_uv_test.py
"""

import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import bpy
import bmesh


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Tools" / "blender"))
sys.path.insert(0, str(ROOT / "tests"))
import io_export_quiver_md5 as addon
from io_export_quiver_md5 import baking
from io_export_quiver_md5.model import ExportError
from blender_md5_bake_test import png_pixels, source_snapshot
from blender_md5_export_test import (create_mesh, image, isolate_bake_cache, parse_mesh, reconstruct,
                                     select_only, texture_path)


def uv_snapshot(obj):
    return (obj.data.uv_layers.active_index,
            tuple((layer.name, layer.active_render, tuple(tuple(corner.uv) for corner in layer.data))
                  for layer in obj.data.uv_layers))


def mesh_snapshot(obj):
    return (tuple(tuple(vertex.co) for vertex in obj.data.vertices),
            tuple(tuple(edge.vertices) for edge in obj.data.edges),
            tuple((tuple(face.vertices), face.material_index) for face in obj.data.polygons),
            tuple(slot.material.as_pointer() if slot.material else None for slot in obj.material_slots),
            uv_snapshot(obj))


def triangle_key(points):
    return tuple(sorted(tuple(round(value, 5) for value in point) for point in points))


def cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def positive_overlap(first, second):
    """Independent strict separating-axis check; common edges are allowed."""
    for triangle in (first, second):
        for a, b in zip(triangle, (*triangle[1:], triangle[0])):
            axis = (a[1] - b[1], b[0] - a[0])
            if math.hypot(*axis) < 1e-10:
                continue
            left = [point[0] * axis[0] + point[1] * axis[1] for point in first]
            right = [point[0] * axis[0] + point[1] * axis[1] for point in second]
            if min(max(left), max(right)) - max(min(left), min(right)) <= 1e-8:
                return False
    return True


def interpolate_export_uv(joints, surface, point):
    positions = reconstruct(joints, surface)
    for triangle in surface["triangles"]:
        a, b, c = (positions[index] for index in triangle)
        denominator = cross(a, b, c)
        if abs(denominator) < 1e-10:
            continue
        second = cross(a, point, c) / denominator
        third = cross(a, b, point) / denominator
        barycentric = (1 - second - third, second, third)
        if min(barycentric) >= -1e-6:
            return tuple(sum(weight * surface["vertices"][index][0][axis]
                             for weight, index in zip(barycentric, triangle)) for axis in range(2))
    raise AssertionError(f"No exported triangle contains point {point}")


class GeneratedUVTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        addon.register()

    @classmethod
    def tearDownClass(cls):
        addon.unregister()

    def setUp(self):
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        self.temp = tempfile.TemporaryDirectory(prefix="quiver-md5-uv-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        isolate_bake_cache(self)

    def material(self, name="Generated UV material"):
        material = bpy.data.materials.new(name)
        material.use_nodes = True
        return material, material.node_tree.nodes.get("Principled BSDF")

    def checker(self, material, shader, colors=((1, 0, 0, 1), (0, 1, 0, 1))):
        tree = material.node_tree
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        checker = tree.nodes.new("ShaderNodeTexChecker")
        checker.inputs["Color1"].default_value = colors[0]
        checker.inputs["Color2"].default_value = colors[1]
        checker.inputs["Scale"].default_value = 2
        tree.links.new(coordinates.outputs["UV"], checker.inputs["Vector"])
        tree.links.new(checker.outputs["Color"], shader.inputs["Base Color"])

    def generated_colors(self, material, shader):
        tree = material.node_tree
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        separate = tree.nodes.new("ShaderNodeSeparateXYZ")
        threshold = tree.nodes.new("ShaderNodeMath")
        threshold.operation = "GREATER_THAN"
        threshold.inputs[1].default_value = .5
        colors = tree.nodes.new("ShaderNodeMixRGB")
        colors.blend_type = "MIX"
        colors.inputs[1].default_value = (1, 0, 0, 1)
        colors.inputs[2].default_value = (0, 0, 1, 1)
        tree.links.new(coordinates.outputs["Generated"], separate.inputs[0])
        tree.links.new(separate.outputs["X"], threshold.inputs[0])
        tree.links.new(threshold.outputs[0], colors.inputs[0])
        tree.links.new(colors.outputs["Color"], shader.inputs["Base Color"])

    def plane(self, material, *, uv_extent=2, missing=False):
        obj = create_mesh("UV plane", [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
                          [(0, 1, 2, 3)], material,
                          [(0, 0), (uv_extent, 0), (uv_extent, uv_extent), (0, uv_extent)])
        if missing:
            obj.data.uv_layers.remove(obj.data.uv_layers.active)
        select_only(obj)
        return obj

    def export(self, obj, name="generated_uv", **kwargs):
        results = self.export_surfaces(obj, name, **kwargs)
        self.assertEqual(len(results), 1)
        return results[0]

    def export_surfaces(self, obj, name="generated_uv", **kwargs):
        path = self.directory / f"{name}.md5mesh"
        options = dict(bake_mode="AUTO", bake_uv_mode="AUTO", bake_size_mode="FIXED", bake_size=64,
                       bake_margin=2, bake_samples=1, forward_axis="POS_X")
        options.update(kwargs)
        before, before_mesh = source_snapshot(), mesh_snapshot(obj)
        addon.exporter.export(bpy.context, str(path), **options)
        self.assertEqual(source_snapshot(), before, "Generated UV export changed source IDs, nodes, or context")
        self.assertEqual(mesh_snapshot(obj), before_mesh, "Generated UV export changed source geometry, materials, or UV maps")
        self.assertTrue(path.with_suffix(".md5anim").is_file())
        joints, surfaces = parse_mesh(path)
        results = []
        for surface in surfaces:
            size, pixel = png_pixels(texture_path(self.directory, surface["shader"]))
            results.append((joints, surface, size, pixel))
        return results

    def assert_uv_atlas(self, surface):
        coordinates = [vertex[0] for vertex in surface["vertices"]]
        for uv in coordinates:
            self.assertTrue(all(math.isfinite(value) and -1e-6 <= value <= 1 + 1e-6 for value in uv))
        triangles = [tuple(coordinates[index] for index in triangle) for triangle in surface["triangles"]]
        for triangle in triangles:
            self.assertGreater(abs(cross(*triangle)), 1e-8)
        for index, triangle in enumerate(triangles):
            for other in triangles[index + 1:]:
                self.assertFalse(positive_overlap(triangle, other), (triangle, other))

    def sample(self, result, point):
        joints, surface, _, pixel = result
        u, engine_v = interpolate_export_uv(joints, surface, point)
        return pixel(u, 1 - engine_v)

    def assert_color(self, actual, expected, tolerance=.08):
        for value, target in zip(actual, expected):
            self.assertAlmostEqual(value, target, delta=tolerance)

    def assert_original_checker_pattern(self, result):
        # Original UVs span 0..2, checker scale is 2, hence four cells per
        # model-space unit. Sampling generated UV directly would give two cells.
        first_row = [self.sample(result, (x, .13, 0)) for x in (.1, .35, .6, .85)]
        second_row = [self.sample(result, (x, .38, 0)) for x in (.1, .35, .6, .85)]
        for row in (first_row, second_row):
            for color in row:
                self.assertTrue((color[0] > .9 and color[1] < .1) or
                                (color[1] > .9 and color[0] < .1), color)
                self.assertGreater(color[3], .99)
            self.assert_color(row[0], row[2])
            self.assert_color(row[1], row[3])
            self.assertGreater(abs(row[0][0] - row[1][0]), .8)
        for first, second in zip(first_row, second_row):
            self.assertGreater(abs(first[0] - second[0]), .8)

    def assert_failed_unchanged(self, obj, name, **kwargs):
        path = self.directory / f"{name}.md5mesh"
        before, before_uv = source_snapshot(), uv_snapshot(obj)
        files = {file.name: file.read_bytes() for file in self.directory.iterdir()}
        with self.assertRaises(ExportError):
            addon.exporter.export(bpy.context, str(path), bake_size=64,
                                  bake_margin=2, bake_samples=1, **kwargs)
        self.assertEqual(source_snapshot(), before)
        self.assertEqual(uv_snapshot(obj), before_uv)
        self.assertEqual({file.name: file.read_bytes() for file in self.directory.iterdir()}, files)

    def test_outside_original_uv_generates_atlas_and_preserves_source_checker(self):
        material, shader = self.material("Outside original UV")
        self.checker(material, shader)
        obj = self.plane(material)
        result = self.export(obj)
        self.assertEqual(result[2], (64, 64))
        self.assert_uv_atlas(result[1])
        self.assert_original_checker_pattern(result)
        self.assert_failed_unchanged(obj, "outside_existing", bake_uv_mode="EXISTING")

    def test_overlapping_uv_generates_separate_islands_for_generated_coordinates(self):
        material, shader = self.material("Overlapping source UV")
        self.generated_colors(material, shader)
        obj = create_mesh("Overlapping quads",
                          [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
                           (2, 0, 0), (3, 0, 0), (3, 1, 0), (2, 1, 0)],
                          [(0, 1, 2, 3), (4, 5, 6, 7)], material,
                          [(0, 0), (1, 0), (1, 1), (0, 1)] * 2)
        select_only(obj)
        result = self.export(obj, "overlap")
        self.assert_uv_atlas(result[1])
        self.assert_color(self.sample(result, (.3, .4, 0)), (1, 0, 0, 1))
        self.assert_color(self.sample(result, (2.3, .4, 0)), (0, 0, 1, 1))
        self.assert_failed_unchanged(obj, "overlap_existing", bake_uv_mode="EXISTING")

    def test_missing_uv_generated_material_bakes_without_adding_source_map(self):
        material, shader = self.material("Missing source UV")
        self.generated_colors(material, shader)
        obj = self.plane(material, missing=True)
        result = self.export(obj, "missing")
        self.assert_uv_atlas(result[1])
        self.assert_color(self.sample(result, (.2, .4, 0)), (1, 0, 0, 1))
        self.assert_color(self.sample(result, (.8, .4, 0)), (0, 0, 1, 1))
        self.assertEqual(len(obj.data.uv_layers), 0)

    def test_missing_uv_explicit_skin_and_bake_off_keep_manual_error(self):
        material, shader = self.material("Manual UV required")
        self.generated_colors(material, shader)
        obj = self.plane(material, missing=True)
        self.assert_failed_unchanged(obj, "off", bake_mode="OFF")
        material.quiver_md5.skins.add().diffuse = image("Explicit manual skin", (1, 0, 0, 1))
        self.assert_failed_unchanged(obj, "explicit")
        self.assertEqual(len(obj.data.uv_layers), 0)

    def test_valid_existing_uv_coordinates_are_retained(self):
        material, shader = self.material("Valid original UV")
        self.checker(material, shader)
        obj = self.plane(material, uv_extent=1)
        original = [tuple(corner.uv) for corner in obj.data.uv_layers.active.data]
        result = self.export(obj, "valid")
        exported = [vertex[0] for vertex in result[1]["vertices"]]
        self.assertEqual(len(exported), 4)
        for u, v in original:
            self.assertTrue(any(abs(ou - u) < 1e-6 and abs(ov - (1 - v)) < 1e-6 for ou, ov in exported))
        self.assert_uv_atlas(result[1])

    def test_generated_uv_interpolates_through_subdivision_modifier(self):
        material, shader = self.material("Subdivided original UV")
        self.checker(material, shader)
        obj = self.plane(material)
        modifier = obj.modifiers.new("Source subdivision", "SUBSURF")
        modifier.subdivision_type = "SIMPLE"
        modifier.levels = 1
        modifier.render_levels = 2
        result = self.export(obj, "subdivision")
        self.assertEqual(len(result[1]["triangles"]), 8)
        positions = reconstruct(result[0], result[1])
        unique = {tuple(round(value, 5) for value in position) for position in positions}
        self.assertEqual(len(unique), 9)
        self.assert_uv_atlas(result[1])
        self.assert_original_checker_pattern(result)
        self.assertEqual(modifier.levels, 1)
        self.assertEqual(modifier.render_levels, 2)

    def test_import_style_alternating_materials_and_unused_slot_without_modifiers(self):
        first, first_shader = self.material("Imported red-green")
        second, second_shader = self.material("Imported blue-yellow")
        unused, _ = self.material("Unused imported slot")
        first.quiver_md5.shader_name = "import_checker_a"
        second.quiver_md5.shader_name = "import_checker_b"
        palettes = (((1, 0, 0, 1), (0, 1, 0, 1)),
                    ((0, 0, 1, 1), (1, 1, 0, 1)))
        self.checker(first, first_shader, palettes[0])
        self.checker(second, second_shader, palettes[1])
        vertices, faces, uvs = [], [], []
        for index in range(4):
            x, base = index * 2, len(vertices)
            vertices.extend([(x, 0, 0), (x + 1, 0, 0), (x + 1, 1, 0), (x, 1, 0)])
            # Imported meshes often start otherwise identical faces at different
            # corners. Keep their original UV-to-position mapping explicit.
            order = [(corner + index) % 4 for corner in range(4)]
            faces.append(tuple(base + corner for corner in order))
            local_uvs = ((0, 0), (2, 0), (2, 2), (0, 2))
            uvs.extend(local_uvs[corner] for corner in order)
        obj = create_mesh("Mesh_0 import regression", vertices, faces, first, uvs)
        obj.data.materials.append(unused)
        obj.data.materials.append(second)
        for polygon, material_index in zip(obj.data.polygons, (2, 0, 2, 0)):
            polygon.material_index = material_index
        select_only(obj)
        self.assertEqual(len(obj.modifiers), 0)
        obj.data.calc_loop_triangles()
        expected = {index: sorted(triangle_key(obj.data.vertices[v].co for v in tri.vertices)
                                  for tri in obj.data.loop_triangles if tri.material_index == index)
                    for index in (0, 2)}
        results = self.export_surfaces(obj, "import_materials")
        self.assertEqual(len(results), 2)
        by_name = {result[1]["shader"].rsplit("/", 1)[-1]: result for result in results}
        phase_colors = {}
        for stem, material_index, palette, starts in (
                ("import_checker_a", 0, palettes[0], (2, 6)),
                ("import_checker_b", 2, palettes[1], (0, 4))):
            result = by_name[stem]
            self.assert_uv_atlas(result[1])
            positions = reconstruct(result[0], result[1])
            actual = sorted(triangle_key(positions[v] for v in tri) for tri in result[1]["triangles"])
            self.assertEqual(actual, expected[material_index])
            colors = []
            for start in starts:
                row = [self.sample(result, (start + x, .13, 0)) for x in (.1, .35, .6, .85)]
                for color in row:
                    self.assertTrue(any(all(abs(value - target) < .1 for value, target in zip(color, choice))
                                        for choice in palette), color)
                self.assert_color(row[0], row[2])
                self.assert_color(row[1], row[3])
                self.assertGreater(sum(abs(a - b) for a, b in zip(row[0][:3], row[1][:3])), 1.5)
                colors.append(row[0])
            self.assert_color(colors[0], colors[1])
            phase_colors[stem] = colors[0]

        # Exercise the same multi-material object without Smart Project too:
        # disjoint valid quadrants and ALWAYS still need each polygon's slot.
        uv_name = obj.data.uv_layers.active.name
        for index, polygon in enumerate(obj.data.polygons):
            for loop_index in polygon.loop_indices:
                position = obj.data.vertices[obj.data.loops[loop_index].vertex_index].co
                obj.data.uv_layers[uv_name].data[loop_index].uv = (
                    (position.x - index * 2) * .4 + .05 + (index % 2) * .5,
                    position.y * .4 + .05 + (index // 2) * .5)
        results = self.export_surfaces(obj, "import_materials", bake_mode="ALWAYS")
        by_name = {result[1]["shader"].rsplit("/", 1)[-1]: result for result in results}
        for stem, material_index, palette, polygon_indices in (
                ("import_checker_a", 0, palettes[0], (1, 3)),
                ("import_checker_b", 2, palettes[1], (0, 2))):
            result = by_name[stem]
            positions = reconstruct(result[0], result[1])
            actual = sorted(triangle_key(positions[v] for v in tri) for tri in result[1]["triangles"])
            self.assertEqual(actual, expected[material_index])
            self.assert_uv_atlas(result[1])
            phase = min(range(2), key=lambda index: sum(abs(a - b) for a, b in zip(palette[index], phase_colors[stem])))
            for polygon_index in polygon_indices:
                parity = ((polygon_index % 2) + (polygon_index // 2)) % 2
                target = palette[phase if parity == 0 else 1 - phase]
                self.assert_color(self.sample(result, (polygon_index * 2 + .3, .4, 0)), target)

    def test_import_style_ngon_cyclic_loops_and_loose_geometry_without_modifiers(self):
        material, shader = self.material("Imported polygon checker")
        self.checker(material, shader)
        # Scrambled vertex indices, a loose edge, an isolated vertex, a cyclic
        # pentagon, and a quad listed before the spatially earlier pentagon.
        vertices = [(-2, -2, 0), (2.4, .8, 0), (0, 0, 0), (-3, -1, 0),
                    (0, 1, 0), (2, 0, 0), (1.2, 1.8, 0), (9, 9, 0),
                    (4, 0, 0), (5, 0, 0), (5, 1, 0), (4, 1, 0)]
        faces = [(10, 11, 8, 9), (6, 4, 2, 5, 1)]
        data = bpy.data.meshes.new("Imported polygon data")
        data.from_pydata(vertices, [(0, 3)], faces)
        data.update()
        obj = bpy.data.objects.new("Mesh_0 polygon regression", data)
        bpy.context.collection.objects.link(obj)
        data.materials.append(material)
        uv_name = data.uv_layers.new(name="Imported UV").name
        for polygon in data.polygons:
            for loop_index in polygon.loop_indices:
                position = data.vertices[data.loops[loop_index].vertex_index].co
                data.uv_layers[uv_name].data[loop_index].uv = (position.x * 2, position.y * 2)
        select_only(obj)
        self.assertEqual(len(obj.modifiers), 0)
        data.calc_loop_triangles()
        expected = sorted(triangle_key(data.vertices[v].co for v in tri.vertices) for tri in data.loop_triangles)
        result = self.export(obj, "import_polygons")
        positions = reconstruct(result[0], result[1])
        actual = sorted(triangle_key(positions[v] for v in tri) for tri in result[1]["triangles"])
        self.assertEqual(actual, expected)
        self.assertEqual(len(result[1]["triangles"]), 5)
        self.assertEqual(len({tuple(round(value, 5) for value in position) for position in positions}), 9)
        self.assert_uv_atlas(result[1])
        self.assert_original_checker_pattern(result)

    def test_uv_overlap_accepts_float32_shared_edge_noise_and_rejects_real_overlap(self):
        material, shader = self.material("Float32 edge checker")
        self.checker(material, shader)
        # M is float32(A + .37 * (B-A)), so it lies on the intended shared
        # edge. Independent float32 rounding puts it only ~1.7e-9 UV units
        # inside the neighbor, which is not a meaningful overlapping island.
        a = (.13238327205181122, .11508491635322571)
        b = (.8301869034767151, .5144872665405273)
        c = (.10000000149011612, .8999999761581421)
        m = (.39057061076164246, .26286378502845764)
        d = (.8999999761581421, .05000000074505806)
        obj = create_mesh("Float32 shared edge", [(0, 0, 0), (1, 0, 0), (0, 1, 0),
                                                 (2, 0, 0), (3, 0, 0), (2, 1, 0)],
                          [(0, 1, 2), (3, 4, 5)], material, [a, b, c, a, m, d])
        select_only(obj)
        before = mesh_snapshot(obj)
        baking._validate_uv(obj.data, 0, obj.data.uv_layers.active.name, obj.name)
        self.assertEqual(mesh_snapshot(obj), before)
        # Moving the shared point clearly into the other triangle must still
        # fail. Increasing texture resolution must never make this acceptable.
        obj.data.uv_layers.active.data[4].uv = (m[0], m[1] + .005)
        before = mesh_snapshot(obj)
        with self.assertRaisesRegex(baking.BakeUVError, "overlap"):
            baking._validate_uv(obj.data, 0, obj.data.uv_layers.active.name, obj.name)
        self.assertEqual(mesh_snapshot(obj), before)
        # Tiny atlas triangles can genuinely overlap by a large fraction of
        # their area. This pair overlaps about 24% of its smaller triangle;
        # its absolute intersection area must not be dismissed as roundoff.
        tiny_a = ((.08616425842046738, .5803098082542419),
                  (.08613043278455734, .5803447365760803),
                  (.08615834265947342, .5802758932113647))
        tiny_b = ((.08612846583127975, .5803185701370239),
                  (.08614634722471237, .5803012847900391),
                  (.08614736050367355, .5803168416023254))
        for corner, uv in zip(obj.data.uv_layers.active.data, (*tiny_a, *tiny_b)):
            corner.uv = uv
        before = mesh_snapshot(obj)
        with self.assertRaisesRegex(baking.BakeUVError, "overlap"):
            baking._validate_uv(obj.data, 0, obj.data.uv_layers.active.name, obj.name)
        self.assertEqual(mesh_snapshot(obj), before)

    def test_duplicate_and_folded_faces_fallback_preserves_source_image_mapping(self):
        material, shader = self.material("Folded imported texture")
        tree = material.node_tree
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        mapping = tree.nodes.new("ShaderNodeMapping")
        texture = tree.nodes.new("ShaderNodeTexImage")
        texture.image = bpy.data.images.new("Original imported checker", width=2, height=2, alpha=True)
        texture.image.pixels = [1, 0, 0, 1, 0, 1, 0, 1,
                                0, 1, 0, 1, 1, 0, 0, 1]
        texture.interpolation = "Closest"
        tree.links.new(coordinates.outputs["UV"], mapping.inputs["Vector"])
        tree.links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
        tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
        base = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        fold = [(1, 0, 0), (1, 0, 1), (1, 1, 1), (1, 1, 0)]
        top = [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
        vertices = [vertex for quad in (base, base, fold, top, fold) for vertex in quad]
        faces = [tuple(range(index * 4, index * 4 + 4)) for index in range(5)]
        obj = create_mesh("Mesh_0 folded duplicate faces", vertices, faces, material,
                          [(0, 0), (2, 0), (2, 2), (0, 2)] * 5)
        select_only(obj)
        self.assertEqual(len(obj.modifiers), 0)
        obj.data.calc_loop_triangles()
        expected = sorted(triangle_key(obj.data.vertices[v].co for v in triangle.vertices)
                          for triangle in obj.data.loop_triangles)
        image_state = (tuple(texture.image.pixels), texture.image.filepath,
                       texture.image.is_dirty, texture.image.packed_file is not None)
        projected, fallback_calls = [], []

        def overlapping_smart_project(session):
            # The injected primary unwrap deliberately stacks all five faces.
            # Only the isolated temporary object's active destination map is
            # touched; material UVs and the user's source mesh stay untouched.
            temporary = bpy.context.edit_object
            self.assertIsNot(temporary, obj)
            bm = bmesh.from_edit_mesh(temporary.data)
            layer = bm.loops.layers.uv.active
            for face in bm.faces:
                for loop, uv in zip(face.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
                    loop[layer].uv = uv
            bmesh.update_edit_mesh(temporary.data, loop_triangles=False, destructive=False)
            projected.append(temporary.as_pointer())
            return {"FINISHED"}

        # A normal export covers real duplicated/coplanar geometry. The second
        # export forces a broken primary projection to exercise the fallback.
        results = [self.export(obj, "folded", bake_size=64)]
        real_fallback = baking.BakeSession._fallback_uv

        def track_fallback(session, *args, **kwargs):
            fallback_calls.append(True)
            return real_fallback(session, *args, **kwargs)

        with mock.patch.object(baking.BakeSession, "_project_primary", new=overlapping_smart_project), \
                mock.patch.object(baking.BakeSession, "_fallback_uv", new=track_fallback):
            results.append(self.export(obj, "folded", bake_size=64))
        self.assertTrue(projected, "The deliberately broken Smart Project was not reached")
        self.assertTrue(fallback_calls, "The overlapping primary projection did not trigger the fallback")
        grid_calls, stacked_fallback_calls = [], []
        real_grid_fallback = baking.BakeSession._grid_fallback_uv

        def leave_fallback_stacked(session, temporary, material_index, uv_name):
            # Keep the primary projection's overlapping UVs so the final
            # disjoint-cell atlas has to handle the same real color bake.
            stacked_fallback_calls.append(True)

        def track_grid_fallback(session, *args, **kwargs):
            grid_calls.append(True)
            return real_grid_fallback(session, *args, **kwargs)

        with mock.patch.object(baking.BakeSession, "_project_primary", new=overlapping_smart_project), \
                mock.patch.object(baking.BakeSession, "_fallback_uv", new=leave_fallback_stacked), \
                mock.patch.object(baking.BakeSession, "_grid_fallback_uv", new=track_grid_fallback):
            results.append(self.export(obj, "folded", bake_size=64))
        self.assertTrue(stacked_fallback_calls, "The invalid second packing stage was not reached")
        self.assertTrue(grid_calls, "Two overlapping packing stages did not trigger the final grid atlas")
        for result in results:
            self.assert_uv_atlas(result[1])
            self.assert_original_checker_pattern(result)
            positions = reconstruct(result[0], result[1])
            actual = sorted(triangle_key(positions[v] for v in triangle) for triangle in result[1]["triangles"])
            self.assertEqual(actual, expected)
        self.assertEqual((tuple(texture.image.pixels), texture.image.filepath,
                          texture.image.is_dirty, texture.image.packed_file is not None), image_state)

    def test_temporary_cleanup_removes_zero_area_face_and_preserves_valid_material_uv(self):
        discarded, _ = self.material("Degenerate-only material")
        discarded.quiver_md5.shader_name = "discarded_zero_area"
        valid, shader = self.material("Valid second material")
        valid.quiver_md5.shader_name = "valid_after_cleanup"
        self.checker(valid, shader)
        obj = create_mesh("Mesh_0 zero-area face", [(0, 0, 0), (1, 0, 0), (0, 1, 0),
                                                   (2, 0, 0), (2.5, 0, 0), (3, 0, 0)],
                          [(0, 1, 2), (3, 4, 5)], discarded,
                          [(0, 0), (2, 0), (0, 2), (0, 0), (1, 0), (1, 1)])
        obj.data.materials.append(valid)
        obj.data.polygons[0].material_index = 1
        obj.data.polygons[1].material_index = 0
        select_only(obj)
        result = self.export(obj, "cleaned")
        self.assertTrue(result[1]["shader"].endswith("/valid_after_cleanup"))
        self.assertEqual(len(result[1]["triangles"]), 1)
        positions = reconstruct(result[0], result[1])
        self.assertEqual({tuple(round(value, 5) for value in point) for point in positions},
                         {(0, 0, 0), (1, 0, 0), (0, 1, 0)})
        self.assert_uv_atlas(result[1])
        first = self.sample(result, (.1, .13, 0))
        second = self.sample(result, (.35, .13, 0))
        for color in (first, second):
            self.assertTrue((color[0] > .9 and color[1] < .1) or
                            (color[1] > .9 and color[0] < .1), color)
        self.assertGreater(abs(first[0] - second[0]), .8)
        self.assertFalse(list(self.directory.glob("discarded_zero_area*.png")))

    def test_all_degenerate_geometry_fails_clearly_without_changing_source_or_output(self):
        material, shader = self.material("All degenerate checker")
        self.checker(material, shader)
        obj = create_mesh("All degenerate Mesh_0", [(0, 0, 0), (1, 0, 0), (2, 0, 0),
                                                   (0, 1, 0), (0, 1, 0), (1, 1, 0)],
                          [(0, 1, 2), (3, 4, 5)], material,
                          [(0, 0), (2, 0), (0, 2)] * 2)
        select_only(obj)
        before, before_mesh = source_snapshot(), mesh_snapshot(obj)
        with self.assertRaisesRegex(ExportError, "degenerate|zero.area|no.*(?:faces|triangles|geometry)"):
            addon.exporter.export(bpy.context, str(self.directory / "all_degenerate.md5mesh"),
                                  bake_size=64, bake_margin=2, bake_samples=1)
        self.assertEqual(source_snapshot(), before)
        self.assertEqual(mesh_snapshot(obj), before_mesh)
        self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(GeneratedUVTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
