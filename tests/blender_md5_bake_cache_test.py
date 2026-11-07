# SPDX-License-Identifier: GPL-2.0-or-later
"""Real CPU Cycles checks of automatic bake sizing and bake reuse.

    blender --background --factory-startup --python-exit-code 1 \
        --python tests/blender_md5_bake_cache_test.py
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import bmesh
import bpy


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Tools" / "blender"))
sys.path.insert(0, str(ROOT / "tests"))
import io_export_quiver_md5 as addon
from io_export_quiver_md5 import bake_cache, bake_inputs, baking
from io_export_quiver_md5.model import ExportError
from io_export_quiver_md5.sizing import GENERATED_UV_COVERAGE
from blender_md5_bake_test import png_pixels, source_snapshot
from blender_md5_export_test import create_mesh, image, parse_mesh, select_only, texture_path


QUAD = [(0, 1, 2, 3)]
UNIT_UVS = [(0, 0), (1, 0), (1, 1), (0, 1)]


def square(extent=1, offset=(0, 0, 0)):
    x, y, z = offset
    return [(x, y, z), (x + extent, y, z), (x + extent, y + extent, z), (x, y + extent, z)]


class CacheTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        addon.register()

    @classmethod
    def tearDownClass(cls):
        addon.unregister()

    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.temp = tempfile.TemporaryDirectory(prefix="quiver-md5-cache-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name) / "out"
        self.cache = Path(self.temp.name) / "cache"
        self.exports = 0

    def material(self, name="Cached"):
        material = bpy.data.materials.new(name)
        material.use_nodes = True
        return material, material.node_tree.nodes.get("Principled BSDF")

    def checker(self, material, shader, coordinate="UV"):
        tree = material.node_tree
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        checker = tree.nodes.new("ShaderNodeTexChecker")
        checker.inputs["Color1"].default_value = (1, 0, 0, 1)
        checker.inputs["Color2"].default_value = (0, 1, 0, 1)
        tree.links.new(coordinates.outputs[coordinate], checker.inputs["Vector"])
        tree.links.new(checker.outputs["Color"], shader.inputs["Base Color"])
        return checker

    def plane(self, material=None, extent=1, uvs=UNIT_UVS, name="Plane", offset=(0, 0, 0)):
        if material is None:
            material, shader = self.material()
            self.checker(material, shader)
        obj = create_mesh(name, square(extent, offset), QUAD, material, uvs)
        select_only(obj)
        return obj

    def two_slot_mesh(self):
        """Two separate quads on one mesh, each with its own material."""
        first, shader = self.material("First")
        self.first_checker = self.checker(first, shader)
        second, shader = self.material("Second")
        self.second_checker = self.checker(second, shader)
        vertices = [*square(1), *square(1, (3, 0, 0))]
        obj = create_mesh("Two slots", vertices, [(0, 1, 2, 3), (4, 5, 6, 7)], first, [*UNIT_UVS, *UNIT_UVS])
        obj.data.materials.append(second)
        obj.data.polygons[1].material_index = 1
        select_only(obj)
        return obj

    def export(self, name="model", **kwargs):
        options = dict(bake_mode="ALWAYS", bake_size_mode="FIXED", bake_size=32, bake_margin=2, bake_samples=1,
                       bake_cache_dir=str(self.cache), selected_only=True)
        options.update(kwargs)
        self.exports += 1
        self.directory.mkdir(exist_ok=True)
        return addon.exporter.export(bpy.context, str(self.directory / f"{name}.md5mesh"), **options)

    def statuses(self, result):
        return sorted(record.status for record in result.bakes)

    def assert_reused(self, label="", **kwargs):
        result = self.export(**kwargs)
        self.assertEqual(set(self.statuses(result)), {"cached"}, label)
        return result

    def assert_rebaked(self, label="", **kwargs):
        result = self.export(**kwargs)
        self.assertEqual(set(self.statuses(result)), {"baked"}, label)
        return result


class SizingTests(CacheTestCase):
    def auto(self, **kwargs):
        options = dict(bake_size_mode="AUTO", bake_texel_density=1, bake_min_size=32, bake_max_size=4096)
        options.update(kwargs)
        return self.export(**options)

    def test_area_sets_the_written_png_size(self):
        cases = ((64, 64), (128, 128), (256, 256))
        for extent, expected in cases:
            with self.subTest(extent):
                obj = self.plane(extent=extent, name=f"Plane {extent}")
                result = self.auto(name=f"plane_{extent}")
                self.assertEqual(result.bakes[0].size, expected)
                self.assertAlmostEqual(result.bakes[0].world_area, extent ** 2, places=2)
                _, surfaces = parse_mesh(self.directory / f"plane_{extent}.md5mesh")
                self.assertEqual(png_pixels(texture_path(self.directory, surfaces[0]["shader"]))[0], (expected, expected))
                obj.select_set(False)

    def test_object_scale_and_export_scale_both_count(self):
        obj = self.plane(extent=1)
        obj.scale = (64, 64, 1)
        bpy.context.view_layer.update()
        self.assertEqual(self.auto().bakes[0].size, 64)
        self.assertEqual(self.auto(scale=2).bakes[0].size, 128)

    def test_rotation_and_mirroring_do_not_change_area(self):
        obj = self.plane(extent=64)
        obj.rotation_euler = (.4, .8, 1.2)
        obj.scale = (-1, 1, 1)
        bpy.context.view_layer.update()
        self.assertAlmostEqual(self.auto().bakes[0].world_area, 64 ** 2, places=1)

    def test_existing_layout_coverage_raises_the_size(self):
        quarter_uvs = [(0, 0), (.5, 0), (.5, .5), (0, .5)]
        self.plane(extent=64, uvs=quarter_uvs)
        record = self.auto().bakes[0]
        self.assertAlmostEqual(record.uv_coverage, .25)
        self.assertEqual(record.size, 128)
        self.assertAlmostEqual(record.density, 1)

    def generated_plane(self, extent=64):
        return self.plane(extent=extent, uvs=[(0, 0), (2, 0), (2, 2), (0, 2)])  # Outside the tile: AUTO regenerates.

    def test_generated_layouts_are_sized_from_their_measured_coverage(self):
        self.generated_plane()
        record = self.auto().bakes[0]
        self.assertAlmostEqual(record.uv_coverage, 1, delta=.1)  # One quad fills its bounds.
        self.assertEqual(record.size, 64)
        self.assertAlmostEqual(record.density, 1, delta=.1)

    def test_unchanged_generated_surface_repeats_no_search(self):
        """A warm export must not wander to new sizes because it knows more than the cold one."""
        self.generated_plane(extent=256)
        sizes = []
        real = baking.BakeSession.generated_layout

        def recording(session, *args, **kwargs):
            sizes.append(kwargs["size"])
            return real(session, *args, **kwargs)

        def coverage(*_):
            # Uneven, as real layouts are. A search restarted from these results
            # would begin at 512 and go on to probe 2048.
            return {256: .1, 512: .02, 1024: .2, 2048: .5}[sizes[-1]]

        with mock.patch.object(bake_inputs, "tuple_coverage", side_effect=coverage):
            with mock.patch.object(baking.BakeSession, "generated_layout", new=recording):
                self.assertEqual(self.statuses(self.auto()), ["baked"])
            self.assertGreater(len(set(sizes)), 1)
            with mock.patch.object(baking.BakeSession, "generated_layout", side_effect=AssertionError("Projected")):
                self.assertEqual(self.statuses(self.auto()), ["cached"])

    def test_density_edit_reuses_coverage_measurements(self):
        self.generated_plane()
        self.auto()
        with mock.patch.object(baking.BakeSession, "generated_layout", side_effect=AssertionError("Projected")), \
                mock.patch.object(baking.BakeSession, "bake", side_effect=AssertionError("Baked")):
            self.assertEqual(self.statuses(self.auto(bake_texel_density=1.1)), ["cached"])

    def test_material_edit_reuses_coverage_measurements(self):
        material, shader = self.material()
        checker = self.checker(material, shader)
        self.plane(material, extent=64, uvs=[(0, 0), (2, 0), (2, 2), (0, 2)])
        self.auto()
        checker.inputs["Scale"].default_value = 7
        with mock.patch.object(baking.BakeSession, "generated_layout", side_effect=AssertionError("Projected")):
            self.assertEqual(self.statuses(self.auto()), ["baked"])

    def test_each_material_slot_is_sized_from_its_own_faces(self):
        first, shader = self.material("Big")
        self.checker(first, shader)
        second, shader = self.material("Small")
        self.checker(second, shader)
        obj = create_mesh("Sizes", [*square(256), *square(64, (300, 0, 0))], [(0, 1, 2, 3), (4, 5, 6, 7)], first,
                          [*UNIT_UVS, *UNIT_UVS])
        obj.data.materials.append(second)
        obj.data.polygons[1].material_index = 1
        select_only(obj)
        self.assertEqual([record.size for record in self.auto().bakes], [256, 64])

    def test_limits_and_warning(self):
        self.plane(extent=1024)
        result = self.auto(bake_max_size=256)
        self.assertEqual(result.bakes[0].size, 256)
        self.assertTrue(any("Max Size" in warning for warning in result.warnings), result.warnings)
        self.assertEqual(self.auto(bake_max_size=256, bake_texel_density=.001, bake_min_size=64).bakes[0].size, 64)

    def test_material_override_beats_area_and_fixed_mode(self):
        material, shader = self.material()
        self.checker(material, shader)
        material.quiver_md5.bake_size_override = 48
        self.plane(material, extent=512)
        self.assertEqual(self.auto().bakes[0].size, 48)
        self.assertEqual(self.export(bake_size=64).bakes[0].size, 48)
        material.quiver_md5.bake_size_override = 8
        with self.assertRaisesRegex(ExportError, "Material Bake Size"):
            self.auto()

    def test_margin_is_limited_by_small_automatic_sizes(self):
        self.plane(extent=1)
        result = self.auto(bake_margin=64, bake_min_size=32)
        self.assertEqual(result.bakes[0].size, 32)

    def test_operator_defaults_to_automatic_sizing(self):
        self.plane(extent=16)  # 16 units at the default density of 4 is 64 pixels.
        self.directory.mkdir()
        status = bpy.ops.export_scene.quiver_md5(filepath=str(self.directory / "op.md5mesh"), bake_mode="ALWAYS",
                                                 bake_margin=2, bake_samples=1, bake_cache_dir=str(self.cache))
        self.assertEqual(status, {"FINISHED"})
        _, surfaces = parse_mesh(self.directory / "op.md5mesh")
        self.assertEqual(png_pixels(texture_path(self.directory, surfaces[0]["shader"]))[0], (64, 64))


class ReuseTests(CacheTestCase):
    def test_unchanged_export_never_opens_a_bake_session_and_matches_bytes(self):
        self.plane()
        result = self.export()
        self.assertEqual(self.statuses(result), ["baked"])
        png = texture_path(self.directory, parse_mesh(self.directory / "model.md5mesh")[1][0]["shader"])
        first = png.read_bytes(), (self.directory / "model.md5mesh").read_bytes()
        before = source_snapshot()
        with mock.patch.object(baking.BakeSession, "__enter__", side_effect=AssertionError("Opened a bake session")):
            self.assert_reused()
        self.assertEqual(source_snapshot(), before)
        self.assertEqual((png.read_bytes(), (self.directory / "model.md5mesh").read_bytes()), first)

    def test_generated_uvs_come_back_from_the_cache(self):
        material, shader = self.material()
        self.checker(material, shader, "Generated")
        cube = [(x, y, z) for x in (0, 1) for y in (0, 1) for z in (0, 1)]
        faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        obj = create_mesh("Cube", cube, faces, material)
        obj.data.uv_layers.remove(obj.data.uv_layers.active)
        select_only(obj)
        self.export(bake_size=64)
        path = self.directory / "model.md5mesh"
        first = path.read_bytes()
        path.unlink()
        self.assert_reused(bake_size=64)
        self.assertEqual(path.read_bytes(), first)

    def test_glow_is_cached_with_the_diffuse(self):
        material, shader = self.material()
        self.checker(material, shader)
        shader.inputs["Emission Color"].default_value = (1, 0, 0, 1)
        shader.inputs["Emission Strength"].default_value = 1
        self.plane(material)
        self.export()
        stem = parse_mesh(self.directory / "model.md5mesh")[1][0]["shader"]
        glow = texture_path(self.directory, stem, suffix="_glow")
        expected = glow.read_bytes()
        glow.unlink()
        self.assert_reused()
        self.assertEqual(glow.read_bytes(), expected)

    def test_inputs_that_change_the_bake_all_miss(self):
        material, shader = self.material()
        checker = self.checker(material, shader)
        ramp = material.node_tree.nodes.new("ShaderNodeValToRGB")
        curve = material.node_tree.nodes.new("ShaderNodeRGBCurve")
        mapping = material.node_tree.nodes.new("ShaderNodeMapping")
        links = material.node_tree.links
        links.new(checker.outputs["Fac"], ramp.inputs["Fac"])
        links.new(ramp.outputs["Color"], curve.inputs["Color"])
        links.new(curve.outputs["Color"], shader.inputs["Base Color"])
        coordinates = next(n for n in material.node_tree.nodes if n.type == "TEX_COORD")
        links.new(coordinates.outputs["UV"], mapping.inputs["Vector"])
        links.new(mapping.outputs["Vector"], checker.inputs["Vector"])
        group = bpy.data.node_groups.new("Tint", "ShaderNodeTree")
        group.interface.new_socket(name="Color", in_out="INPUT", socket_type="NodeSocketColor")
        group.interface.new_socket(name="Color", in_out="OUTPUT", socket_type="NodeSocketColor")
        inner = group.nodes.new("ShaderNodeInvert")
        group_in, group_out = group.nodes.new("NodeGroupInput"), group.nodes.new("NodeGroupOutput")
        group.links.new(group_in.outputs[0], inner.inputs["Color"])
        group.links.new(inner.outputs[0], group_out.inputs[0])
        node = material.node_tree.nodes.new("ShaderNodeGroup")
        node.node_tree = group
        links.new(curve.outputs["Color"], node.inputs["Color"])
        links.new(node.outputs["Color"], shader.inputs["Base Color"])
        obj = self.plane(material)
        self.export()
        self.assert_reused("baseline")

        edits = {
            "socket value": lambda: checker.inputs["Scale"].__setattr__("default_value", 9),
            "color ramp stop": lambda: setattr(ramp.color_ramp.elements[1], "position", .7),
            "ramp interpolation": lambda: setattr(ramp.color_ramp, "interpolation", "CONSTANT"),
            "curve point": lambda: setattr(curve.mapping.curves[3].points[0], "location", (0, .2)),
            "mapping": lambda: mapping.inputs["Location"].__setattr__("default_value", (.1, 0, 0)),
            "hidden texture mapping": lambda: setattr(checker.texture_mapping, "translation", (.3, 0, 0)),
            "muted node": lambda: setattr(curve, "mute", True),
            "inside a group": lambda: inner.inputs["Fac"].__setattr__("default_value", .5),
            "alpha": lambda: shader.inputs["Alpha"].__setattr__("default_value", .5),
            "emission": lambda: shader.inputs["Emission Strength"].__setattr__("default_value", 1),
            "vertex": lambda: obj.data.vertices[0].co.__setitem__(2, .25),
            "uv": lambda: obj.data.uv_layers[0].data[0].__setattr__("uv", (.1, .1)),
            "rewired": lambda: links.new(ramp.outputs["Color"], node.inputs["Color"]),
        }
        for label, edit in edits.items():
            with self.subTest(label):
                edit()
                obj.data.update()
                self.assert_rebaked(label)
                self.assert_reused(label)

    def test_settings_that_change_the_bake_all_miss(self):
        self.plane()
        self.export()
        for label, options in (("size", dict(bake_size=64)), ("margin", dict(bake_margin=3)),
                               ("samples", dict(bake_samples=2)), ("uv mode", dict(bake_uv_mode="EXISTING"))):
            with self.subTest(label):
                self.assert_rebaked(label, **options)
                self.assert_reused(label, **options)

    def test_density_changes_rebake_only_when_the_size_changes(self):
        self.plane(extent=64)
        options = dict(bake_size_mode="AUTO", bake_min_size=32, bake_max_size=1024)
        self.assertEqual(self.export(bake_texel_density=1, **options).bakes[0].size, 64)
        self.assert_reused(bake_texel_density=1.2, **options)
        self.assertEqual(self.assert_rebaked(bake_texel_density=2, **options).bakes[0].size, 128)

    def test_image_pixels_miss_even_with_the_same_datablock(self):
        material, shader = self.material()
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image("Source", (1, 0, 0, 1))
        material.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
        self.plane(material)
        self.export()
        self.assert_reused()
        texture.image.pixels[0] = .25
        texture.image.update()
        self.assert_rebaked("pixel edit")
        texture.image.colorspace_settings.name = "Non-Color"
        self.assert_rebaked("colorspace")

    def test_cosmetic_edits_still_hit(self):
        material, shader = self.material()
        checker = self.checker(material, shader)
        obj = self.plane(material)
        self.export()
        checker.location = (500, 500)
        checker.select = True
        checker.label = "Renamed"
        material.name = "A new name"
        obj.name = "Another name"
        obj.data.name = "Mesh name"
        material.node_tree.nodes.new("ShaderNodeTexNoise")  # Connected to nothing.
        obj.location = (10, 20, 30)
        bpy.context.scene.frame_set(40)
        bpy.context.view_layer.update()
        self.assert_reused("cosmetic")

    def test_editing_one_slot_rebakes_only_that_slot(self):
        obj = self.two_slot_mesh()
        self.export()
        self.first_checker.inputs["Scale"].default_value = 11
        self.assertEqual(self.statuses(self.export()), ["baked", "cached"], "material edit")
        obj.data.vertices[5].co.z = .5
        obj.data.update()
        self.assertEqual(self.statuses(self.export()), ["baked", "cached"], "geometry edit")

    def test_added_faces_elsewhere_keep_other_slots(self):
        obj = self.two_slot_mesh()
        self.export()
        editable = bmesh.new()
        editable.from_mesh(obj.data)
        editable.faces.ensure_lookup_table()
        second_face = editable.faces[1]
        face = editable.faces.new([editable.verts.new(point) for point in square(1, (6, 0, 0))])
        face.material_index = 1
        layer = editable.loops.layers.uv.active
        for corner, uv in zip(face.loops, [(0, 0), (.5, 0), (.5, .5), (0, .5)]):
            corner[layer].uv = uv
        for corner, uv in zip(second_face.loops, [(.5, .5), (1, .5), (1, 1), (.5, 1)]):
            corner[layer].uv = uv
        editable.to_mesh(obj.data)
        editable.free()
        obj.data.update()
        self.assertEqual(self.statuses(self.export()), ["baked", "cached"])

    def test_generated_coordinates_follow_the_rest_of_the_mesh(self):
        """A bounds change shifts Generated coordinates on faces it never touched."""
        obj = self.two_slot_mesh()
        first = obj.data.materials[0]
        coordinates = next(n for n in first.node_tree.nodes if n.type == "TEX_COORD")
        first.node_tree.links.new(coordinates.outputs["Generated"], self.first_checker.inputs["Vector"])
        self.export()
        obj.data.vertices[5].co.x = 9
        obj.data.update()
        self.assertEqual(self.statuses(self.export()), ["baked", "baked"])

    def test_unlinked_procedural_vector_counts_as_generated(self):
        obj = self.two_slot_mesh()
        first = obj.data.materials[0]
        for link in list(self.first_checker.inputs["Vector"].links):
            first.node_tree.links.remove(link)
        self.export()
        obj.data.vertices[5].co.x = 9
        obj.data.update()
        self.assertEqual(self.statuses(self.export()), ["baked", "baked"])

    def test_identical_surfaces_bake_once(self):
        material, shader = self.material()
        self.checker(material, shader)
        first = self.plane(material, name="First")
        second = first.copy()
        second.location = (5, 0, 0)
        bpy.context.collection.objects.link(second)
        select_only(first, second)
        bpy.context.view_layer.update()
        result = self.export()
        self.assertEqual(self.statuses(result), ["baked", "shared"])
        _, surfaces = parse_mesh(self.directory / "model.md5mesh")
        paths = [texture_path(self.directory, surface["shader"]) for surface in surfaces]
        self.assertEqual(len(set(paths)), 2)
        self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())

    def test_shared_stem_is_allowed_when_surfaces_are_identical(self):
        material, shader = self.material()
        self.checker(material, shader)
        material.quiver_md5.shader_name = "shared_skin"
        first = self.plane(material, name="First")
        second = first.copy()
        bpy.context.collection.objects.link(second)
        select_only(first, second)
        self.assertEqual(self.statuses(self.export()), ["baked", "shared"])

    def test_world_position_keeps_surfaces_apart(self):
        material, shader = self.material()
        tree = material.node_tree
        geometry = tree.nodes.new("ShaderNodeNewGeometry")
        tree.links.new(geometry.outputs["Position"], shader.inputs["Base Color"])
        first = self.plane(material, name="First")
        second = first.copy()
        second.location = (5, 0, 0)
        bpy.context.collection.objects.link(second)
        select_only(first, second)
        bpy.context.view_layer.update()
        self.assertEqual(self.statuses(self.export()), ["baked", "baked"])
        second.location = (7, 0, 0)
        bpy.context.view_layer.update()
        self.assertEqual(self.statuses(self.export()), ["baked", "cached"])

    def test_object_properties_read_by_nodes_miss(self):
        material, shader = self.material()
        info = material.node_tree.nodes.new("ShaderNodeObjectInfo")
        material.node_tree.links.new(info.outputs["Color"], shader.inputs["Base Color"])
        obj = self.plane(material)
        self.export()
        obj.color = (0, 1, 0, 1)
        self.assert_rebaked("object color")

    def test_texture_coordinate_target_object_movement_misses(self):
        material, shader = self.material()
        self.checker(material, shader, "Object")
        target = bpy.data.objects.new("Target", None)
        bpy.context.collection.objects.link(target)
        next(n for n in material.node_tree.nodes if n.type == "TEX_COORD").object = target
        self.plane(material)
        self.export()
        target.location = (.3, 0, 0)
        bpy.context.view_layer.update()
        self.assert_rebaked("target moved")

    def test_modifier_change_misses_and_stack_reuse_hits(self):
        obj = self.plane()
        modifier = obj.modifiers.new("Displace", "DISPLACE")
        modifier.strength = .1
        self.export()
        self.assert_reused("same stack")
        modifier.strength = .2
        self.assert_rebaked("strength")

    def test_animated_material_tracks_the_frame(self):
        material, shader = self.material()
        checker = self.checker(material, shader)
        socket = checker.inputs["Scale"]
        socket.default_value = 2
        socket.keyframe_insert("default_value", frame=1)
        socket.default_value = 8
        socket.keyframe_insert("default_value", frame=20)
        self.plane(material)
        bpy.context.scene.frame_set(1)
        self.export()
        self.assert_reused("same frame")
        bpy.context.scene.frame_set(10)
        self.assert_rebaked("new frame")

    def test_image_sequences_are_never_stored(self):
        material, shader = self.material()
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image("Sequence", (1, 0, 0, 1))
        material.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
        self.plane(material)

        def as_sequence(hasher, value):
            hasher.volatile = True
            return ["volatile", "SEQUENCE"]

        with mock.patch.object(bake_inputs.MaterialHasher, "_image", new=as_sequence):
            result = self.export()
            self.assertTrue(any("rebaked on every export" in warning for warning in result.warnings))
            self.assert_rebaked("volatile")
        self.assertEqual(bake_cache.BakeCache(self.cache).usage()[0], 0)

    def test_force_rebake_replaces_the_entry(self):
        self.plane()
        self.export()
        calls = []
        real = baking.BakeSession.bake

        def counted(session, *args, **kwargs):
            calls.append(True)
            return real(session, *args, **kwargs)

        with mock.patch.object(baking.BakeSession, "bake", new=counted):
            self.assertEqual(self.statuses(self.export(force_rebake=True)), ["baked"])
        self.assertEqual(len(calls), 1)
        self.assert_reused("forced result is reusable")

    def test_force_rebake_still_shares_identical_surfaces(self):
        material, shader = self.material()
        self.checker(material, shader)
        first = self.plane(material, name="First")
        second = first.copy()
        bpy.context.collection.objects.link(second)
        select_only(first, second)
        self.export()
        self.assertEqual(self.statuses(self.export(force_rebake=True)), ["baked", "shared"])

    def test_force_rebake_operator_flag_does_not_stick(self):
        self.plane()
        self.directory.mkdir()
        options = dict(filepath=str(self.directory / "op.md5mesh"), bake_mode="ALWAYS", bake_size_mode="FIXED",
                       bake_size=32, bake_margin=2, bake_samples=1, bake_cache_dir=str(self.cache))
        self.assertEqual(bpy.ops.export_scene.quiver_md5(force_rebake=True, **options), {"FINISHED"})
        with mock.patch.object(baking.BakeSession, "bake", side_effect=AssertionError("Rebaked")):
            self.assertEqual(bpy.ops.export_scene.quiver_md5(**options), {"FINISHED"})

    def corrupt(self, result):
        directory = next(self.cache.rglob(result.bakes[0].key))
        path = directory / "diffuse.png"
        data = bytearray(path.read_bytes())
        data[len(data) // 2] ^= 255
        path.write_bytes(bytes(data))

    def test_damaged_entry_is_rebaked_and_reported(self):
        self.plane()
        self.corrupt(self.export())
        result = self.assert_rebaked("corrupt")
        self.assertTrue(any("damaged" in warning for warning in result.warnings), result.warnings)
        self.assert_reused("repaired")

    def test_unwritable_cache_still_exports_and_warns(self):
        self.plane()
        self.cache.parent.mkdir(exist_ok=True)
        self.cache.write_text("a file where the cache directory should be")
        result = self.export()
        self.assertEqual(self.statuses(result), ["baked"])
        self.assertTrue(any("Could not store" in warning for warning in result.warnings), result.warnings)
        _, surfaces = parse_mesh(self.directory / "model.md5mesh")
        self.assertEqual(png_pixels(texture_path(self.directory, surfaces[0]["shader"]))[0], (32, 32))

    def test_new_blender_version_or_code_misses(self):
        self.plane()
        self.export()
        with mock.patch.object(bake_inputs, "code_digest", return_value="different"):
            self.assert_rebaked("code")

    def test_failed_export_keeps_finished_bakes(self):
        obj = self.two_slot_mesh()
        second = obj.data.materials[1]
        mix = second.node_tree.nodes.new("ShaderNodeMixShader")
        output = second.node_tree.nodes.get("Material Output")
        second.node_tree.links.new(mix.outputs[0], output.inputs["Surface"])
        with self.assertRaises(ExportError):
            self.export()
        second.node_tree.links.new(second.node_tree.nodes.get("Principled BSDF").outputs[0], output.inputs["Surface"])
        self.assertEqual(self.statuses(self.export()), ["baked", "cached"])

    def test_clear_operator_empties_the_cache(self):
        self.plane()
        self.export()
        self.assertEqual(bpy.ops.quiver.md5_clear_bake_cache(directory=str(self.cache)), {"FINISHED"})
        self.assert_rebaked("cleared")

    def test_cache_survives_a_blender_restart(self):
        """Key every data type in two real processes, reading a saved .blend."""
        material, shader = self.material()
        self.checker(material, shader)
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image("Packed", (0, 0, 1, 1))
        mix = material.node_tree.nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        links = material.node_tree.links
        checker = next(n for n in material.node_tree.nodes if n.type == "TEX_CHECKER")
        links.new(checker.outputs["Color"], mix.inputs["A"])
        links.new(texture.outputs["Color"], mix.inputs["B"])
        links.new(mix.outputs["Result"], shader.inputs["Base Color"])
        info = material.node_tree.nodes.new("ShaderNodeObjectInfo")  # Brings object state into the key.
        links.new(info.outputs["Color"], shader.inputs["Emission Color"])
        obj = self.plane(material)
        obj.data.color_attributes.new("Col", "BYTE_COLOR", "CORNER")
        obj.data.attributes.new("weight", "FLOAT", "POINT")
        obj["settings"] = {"nested": 1, "values": [1.5, 2.5]}
        obj.modifiers.new("Displace", "DISPLACE").strength = .01
        blend = Path(self.temp.name) / "scene.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(blend))
        script = Path(self.temp.name) / "child.py"
        script.write_text(CHILD_SCRIPT, encoding="utf-8")
        reports = []
        for name in ("first", "second"):
            report = Path(self.temp.name) / f"{name}.json"
            environment = dict(os.environ, PYTHONHASHSEED="random", QUIVER_TEST_ROOT=str(ROOT),
                               QUIVER_TEST_CACHE=str(self.cache), QUIVER_TEST_OUT=str(self.directory),
                               QUIVER_TEST_REPORT=str(report))
            subprocess.run([bpy.app.binary_path, "--background", "--factory-startup", str(blend),
                            "--python-exit-code", "1", "--python", str(script)],
                           check=True, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            reports.append(json.loads(report.read_text()))
        self.assertEqual(reports[0]["status"], ["baked"])
        self.assertEqual(reports[1]["status"], ["cached"])
        self.assertEqual(reports[0]["key"], reports[1]["key"])


CHILD_SCRIPT = """
import json, os, sys
from pathlib import Path
import bpy
sys.path.insert(0, str(Path(os.environ['QUIVER_TEST_ROOT']) / 'Tools' / 'blender'))
import io_export_quiver_md5 as addon
addon.register()
out = Path(os.environ['QUIVER_TEST_OUT'])
out.mkdir(exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
result = addon.exporter.export(bpy.context, str(out / 'restart.md5mesh'), bake_mode='ALWAYS', bake_size_mode='FIXED',
                               bake_size=32, bake_margin=2, bake_samples=1,
                               bake_cache_dir=os.environ['QUIVER_TEST_CACHE'])
Path(os.environ['QUIVER_TEST_REPORT']).write_text(json.dumps(
    {'status': [record.status for record in result.bakes], 'key': [record.key for record in result.bakes]}))
"""


if __name__ == "__main__":
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite([loader.loadTestsFromTestCase(SizingTests), loader.loadTestsFromTestCase(ReuseTests)])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
