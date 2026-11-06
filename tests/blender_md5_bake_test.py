# SPDX-License-Identifier: GPL-2.0-or-later
"""Real CPU Cycles material-baking checks using procedural Blender assets.

    blender --background --factory-startup --python-exit-code 1 \
        --python tests/blender_md5_bake_test.py
"""

import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib

import bpy


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Tools" / "blender"))
sys.path.insert(0, str(ROOT / "tests"))
import io_export_quiver_md5 as addon
from io_export_quiver_md5 import baking
from io_export_quiver_md5.model import ExportError
from blender_md5_export_test import create_mesh, image, parse_mesh, select_only, texture_path


def png_pixels(path):
    """Read 8/16-bit RGB(A) PNG independently of Blender's display transforms."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    offset, compressed = 8, bytearray()
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind, chunk = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">2I5B", chunk)
            assert color in (2, 6) and depth in (8, 16), (depth, color)
            assert (compression, filtering, interlace) == (0, 0, 0)
        elif kind == b"IDAT":
            compressed.extend(chunk)
        offset += length + 12
    channels = 4 if color == 6 else 3
    unit, bpp = depth // 8, channels * (depth // 8)
    stride = width * bpp
    raw, previous, decoded = zlib.decompress(compressed), bytearray(stride), bytearray()
    offset = 0
    for _ in range(height):
        filter_type, row = raw[offset], bytearray(raw[offset + 1:offset + 1 + stride])
        offset += stride + 1
        for index in range(stride):
            left = row[index - bpp] if index >= bpp else 0
            up = previous[index]
            diagonal = previous[index - bpp] if index >= bpp else 0
            if filter_type == 0:
                prediction = 0
            elif filter_type == 1:
                prediction = left
            elif filter_type == 2:
                prediction = up
            elif filter_type == 3:
                prediction = (left + up) // 2
            elif filter_type == 4:
                p = left + up - diagonal
                distances = (abs(p - left), abs(p - up), abs(p - diagonal))
                prediction = (left, up, diagonal)[distances.index(min(distances))]
            else:
                raise AssertionError(f"Unsupported PNG filter {filter_type}")
            row[index] = (row[index] + prediction) & 255
        decoded.extend(row)
        previous = row
    maximum = (1 << depth) - 1

    def pixel(u=.5, v=.5):
        x = min(width - 1, max(0, int(u * width)))
        y = height - 1 - min(height - 1, max(0, int(v * height)))
        start = (y * width + x) * bpp
        values = [int.from_bytes(decoded[start + i * unit:start + (i + 1) * unit], "big") / maximum
                  for i in range(channels)]
        # PNG output is sRGB; material socket values are scene-linear.
        values[:3] = [c / 12.92 if c <= .04045 else ((c + .055) / 1.055) ** 2.4 for c in values[:3]]
        if len(values) == 3:
            values.append(1.0)
        return tuple(values)

    return (width, height), pixel


def value_snapshot(value):
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    try:
        return tuple(value)
    except TypeError:
        return str(value)


def source_snapshot():
    """Record source IDs, node graphs, context, and user rendering preferences."""
    scene = bpy.context.scene
    counts = {}
    for name in ("scenes", "objects", "meshes", "materials", "images", "collections", "worlds", "node_groups"):
        counts[name] = tuple(sorted(item.as_pointer() for item in getattr(bpy.data, name)))
    materials = {}
    for material in bpy.data.materials:
        tree = material.node_tree
        if tree:
            nodes = tuple(sorted((node.as_pointer(), node.name, node.type, node.select, tuple(node.location),
                                  tuple((socket.name, value_snapshot(socket.default_value))
                                        for socket in node.inputs if hasattr(socket, "default_value")))
                                 for node in tree.nodes))
            links = tuple(sorted((link.from_node.as_pointer(), link.from_socket.name,
                                  link.to_node.as_pointer(), link.to_socket.name) for link in tree.links))
            materials[material.as_pointer()] = (nodes, links, tree.nodes.active.as_pointer() if tree.nodes.active else None)
    return dict(
        counts=counts, materials=materials, scene=scene.as_pointer(),
        selected=tuple(sorted(obj.as_pointer() for obj in bpy.context.selected_objects)),
        active=bpy.context.view_layer.objects.active.as_pointer() if bpy.context.view_layer.objects.active else None,
        mode=bpy.context.mode,
        render=(scene.render.engine, scene.render.resolution_x, scene.render.resolution_y,
                scene.render.resolution_percentage, scene.render.image_settings.file_format,
                scene.render.image_settings.color_mode, scene.render.image_settings.color_depth,
                scene.render.bake.margin, scene.render.bake.use_clear, scene.render.bake.use_selected_to_active),
        cycles=(scene.cycles.device, scene.cycles.samples, scene.cycles.preview_samples, scene.cycles.use_denoising),
        view=(scene.view_settings.view_transform, scene.view_settings.look,
              scene.view_settings.exposure, scene.view_settings.gamma),
        frame=scene.frame_current,
        images=tuple(sorted((img.as_pointer(), img.filepath, img.is_dirty,
                             img.packed_file is not None) for img in bpy.data.images)),
    )


class BakeIntegrationTests(unittest.TestCase):
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
        self.temp = tempfile.TemporaryDirectory(prefix="quiver-md5-bake-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        scene = bpy.context.scene
        scene.render.engine = "CYCLES"
        scene.cycles.device = "GPU"  # Baking must isolate its CPU choice.
        scene.cycles.samples = 11
        scene.cycles.preview_samples = 7
        scene.cycles.use_denoising = True
        scene.render.bake.margin = 13
        scene.render.bake.use_clear = False
        scene.render.bake.use_selected_to_active = True
        scene.render.resolution_x, scene.render.resolution_y = 1280, 720
        scene.render.resolution_percentage = 37
        scene.render.image_settings.file_format = "JPEG"
        scene.render.image_settings.color_mode = "RGB"
        scene.view_settings.exposure = .7
        scene.view_settings.gamma = 1.2
        scene.frame_set(9)

    def material(self, name="Baked material"):
        material = bpy.data.materials.new(name)
        material.use_nodes = True
        return material, material.node_tree.nodes.get("Principled BSDF")

    def plane(self, material):
        obj = create_mesh("Bake plane", [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
                          [(0, 1, 2, 3)], material, [(0, 0), (1, 0), (1, 1), (0, 1)])
        select_only(obj)
        return obj

    def checker(self, material, shader):
        tree = material.node_tree
        coordinates = tree.nodes.new("ShaderNodeTexCoord")
        checker = tree.nodes.new("ShaderNodeTexChecker")
        checker.inputs["Color1"].default_value = (1, 0, 0, 1)
        checker.inputs["Color2"].default_value = (0, 1, 0, 1)
        checker.inputs["Scale"].default_value = 4
        tree.links.new(coordinates.outputs["UV"], checker.inputs["Vector"])
        tree.links.new(checker.outputs["Color"], shader.inputs["Base Color"])
        return checker

    def export(self, name="baked", operator=False, **kwargs):
        path = self.directory / f"{name}.md5mesh"
        options = dict(bake_mode="AUTO", bake_size=32, bake_margin=2, bake_samples=1)
        options.update(kwargs)
        before = source_snapshot()
        if operator:
            self.assertEqual(bpy.ops.export_scene.quiver_md5(filepath=str(path), **options), {"FINISHED"})
        else:
            addon.exporter.export(bpy.context, str(path), **options)
        self.assertEqual(source_snapshot(), before, "Baking changed source data or user settings")
        self.assertTrue(path.with_suffix(".md5anim").is_file())
        _, surfaces = parse_mesh(path)
        return surfaces[0]["shader"]

    def assert_pixel(self, actual, expected, tolerance=.025):
        for got, wanted in zip(actual, expected):
            self.assertTrue(math.isfinite(got))
            self.assertAlmostEqual(got, wanted, delta=tolerance)

    def test_auto_checker_bakes_base_color_for_fully_metallic_material(self):
        material, shader = self.material("Metallic checker")
        self.checker(material, shader)
        shader.inputs["Metallic"].default_value = 1
        shader.inputs["Roughness"].default_value = 0
        self.plane(material)
        stem = self.export(operator=True)
        size, pixel = png_pixels(texture_path(self.directory, stem))
        self.assertEqual(size, (32, 32))
        colors = [pixel(.125, .125), pixel(.375, .125)]
        self.assertTrue(any(color[0] > .9 and color[1] < .1 for color in colors), colors)
        self.assertTrue(any(color[1] > .9 and color[0] < .1 for color in colors), colors)
        self.assertTrue(all(color[3] > .99 for color in colors))

    def test_mapping_named_source_uv_and_upstream_color_group(self):
        material, shader = self.material("Mapped group")
        obj = self.plane(material)
        source_uv = obj.data.uv_layers.new(name="SourceUV")
        for corner, uv in zip(source_uv.data, [(.25, 0), (1.25, 0), (1.25, 1), (.25, 1)]):
            corner.uv = uv
        obj.data.uv_layers[0].active_render = True
        obj.data.uv_layers.active_index = 0
        tree = material.node_tree
        uv = tree.nodes.new("ShaderNodeUVMap")
        uv.uv_map = "SourceUV"
        mapping = tree.nodes.new("ShaderNodeMapping")
        mapping.inputs["Location"].default_value = (.25, 0, 0)
        texture = tree.nodes.new("ShaderNodeTexImage")
        texture.image = bpy.data.images.new("Mapping source", width=2, height=2, alpha=True)
        texture.image.pixels = [1, 0, 0, 1, 0, 1, 0, 1] * 2
        texture.interpolation = "Closest"
        group = bpy.data.node_groups.new("Color pass-through", "ShaderNodeTree")
        group.interface.new_socket(name="Color", in_out="INPUT", socket_type="NodeSocketColor")
        group.interface.new_socket(name="Color", in_out="OUTPUT", socket_type="NodeSocketColor")
        group_input = group.nodes.new("NodeGroupInput")
        group_output = group.nodes.new("NodeGroupOutput")
        group.links.new(group_input.outputs["Color"], group_output.inputs["Color"])
        group_node = tree.nodes.new("ShaderNodeGroup")
        group_node.node_tree = group
        tree.links.new(uv.outputs["UV"], mapping.inputs["Vector"])
        tree.links.new(mapping.outputs["Vector"], texture.inputs["Vector"])
        tree.links.new(texture.outputs["Color"], group_node.inputs["Color"])
        tree.links.new(group_node.outputs["Color"], shader.inputs["Base Color"])
        stem = self.export("mapped")
        size, pixel = png_pixels(texture_path(self.directory, stem))
        self.assertEqual(size, (32, 32))
        self.assert_pixel(pixel(.125, .5), (0, 1, 0, 1))
        self.assert_pixel(pixel(.625, .5), (1, 0, 0, 1))

    def test_alpha_and_linked_emission_strength_are_preserved_without_lighting(self):
        material, shader = self.material("Alpha and emission")
        shader.inputs["Base Color"].default_value = (.2, .4, .6, 1)
        shader.inputs["Metallic"].default_value = 1
        shader.inputs["Alpha"].default_value = .25
        shader.inputs["Emission Color"].default_value = (1, 0, 0, 1)
        strength = material.node_tree.nodes.new("ShaderNodeMath")
        strength.operation = "MULTIPLY"
        strength.inputs[0].default_value = .25
        strength.inputs[1].default_value = 2
        material.node_tree.links.new(strength.outputs[0], shader.inputs["Emission Strength"])
        self.plane(material)
        stem = self.export("alpha_emission", bake_size=64)
        size, diffuse = png_pixels(texture_path(self.directory, stem))
        self.assertEqual(size, (64, 64))
        self.assert_pixel(diffuse(), (.2, .4, .6, .25))
        _, glow = png_pixels(texture_path(self.directory, stem, suffix="_glow"))
        self.assert_pixel(glow()[:3], (.5, 0, 0))

    def test_always_bakes_direct_solid_material_at_requested_size(self):
        material, shader = self.material("Always solid")
        shader.inputs["Base Color"].default_value = (.2, .4, .6, 1)
        self.plane(material)
        stem = self.export("always", bake_mode="ALWAYS", bake_size=64)
        size, pixel = png_pixels(texture_path(self.directory, stem))
        self.assertEqual(size, (64, 64))
        self.assert_pixel(pixel(), (.2, .4, .6, 1))

    def test_auto_direct_texture_and_constant_skip_baking(self):
        material, shader = self.material("Direct constant")
        obj = self.plane(material)
        with mock.patch.object(baking.BakeSession, "bake", side_effect=AssertionError("Simple material was baked")):
            stem = self.export("constant")
        self.assertEqual(png_pixels(texture_path(self.directory, stem))[0], (1, 1))
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image("Direct texture", (0, 1, 0, 1))
        material.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
        with mock.patch.object(baking.BakeSession, "bake", side_effect=AssertionError("Direct texture was baked")):
            stem = self.export("direct")
        self.assertEqual(png_pixels(texture_path(self.directory, stem))[0], (2, 2))
        self.assertEqual(set(bpy.context.selected_objects), {obj})

    def test_fast_path_alpha_follows_principled_inputs_and_glow_is_opaque(self):
        material, shader = self.material("Direct alpha")
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image("RGBA source", (1, 0, 0, .2))
        links = material.node_tree.links
        links.new(texture.outputs["Color"], shader.inputs["Base Color"])
        self.plane(material)
        with mock.patch.object(baking.BakeSession, "bake", side_effect=AssertionError("Direct alpha was baked")):
            shader.inputs["Alpha"].default_value = 1
            stem = self.export("opaque_source")
            self.assertAlmostEqual(png_pixels(texture_path(self.directory, stem))[1]()[3], 1, delta=.01)
            shader.inputs["Alpha"].default_value = .25
            stem = self.export("constant_alpha")
            self.assertAlmostEqual(png_pixels(texture_path(self.directory, stem))[1]()[3], .25, delta=.01)
            links.new(texture.outputs["Alpha"], shader.inputs["Alpha"])
            stem = self.export("linked_alpha")
            self.assertAlmostEqual(png_pixels(texture_path(self.directory, stem))[1]()[3], .2, delta=.01)
            links.new(texture.outputs["Color"], shader.inputs["Emission Color"])
            shader.inputs["Emission Strength"].default_value = 1
            stem = self.export("opaque_glow")
            self.assertAlmostEqual(png_pixels(texture_path(self.directory, stem, suffix="_glow"))[1]()[3], 1, delta=.01)

    def test_explicit_skin_list_bypasses_always_baking(self):
        material, shader = self.material("Explicit wins")
        self.checker(material, shader)
        material.quiver_md5.skins.add().diffuse = image("Explicit skin", (0, 0, 1, 1))
        self.plane(material)
        with mock.patch.object(baking.BakeSession, "bake", side_effect=AssertionError("Explicit skin was baked")):
            stem = self.export("explicit", bake_mode="ALWAYS")
        size, pixel = png_pixels(texture_path(self.directory, stem))
        self.assertEqual(size, (2, 2))
        self.assert_pixel(pixel(), (0, 0, 1, 1))

    def test_direct_emission_closure_bakes_fullbright_strength(self):
        material, _ = self.material("Emission closure")
        tree = material.node_tree
        emission = tree.nodes.new("ShaderNodeEmission")
        emission.inputs["Color"].default_value = (.2, .4, .6, 1)
        emission.inputs["Strength"].default_value = .5
        tree.links.new(emission.outputs[0], tree.nodes.get("Material Output").inputs["Surface"])
        self.plane(material)
        stem = self.export("emission")
        diffuse_size, diffuse = png_pixels(texture_path(self.directory, stem))
        self.assertEqual(diffuse_size, (32, 32))
        self.assert_pixel(diffuse(), (0, 0, 0, 1))
        size, glow = png_pixels(texture_path(self.directory, stem, suffix="_glow"))
        self.assertEqual(size, (32, 32))
        self.assert_pixel(glow()[:3], (.1, .2, .3))

    def test_off_rejects_procedural_material_without_side_effects(self):
        material, shader = self.material("Baking disabled")
        self.checker(material, shader)
        self.plane(material)
        before = source_snapshot()
        with self.assertRaises(ExportError):
            addon.exporter.export(bpy.context, str(self.directory / "off.md5mesh"), bake_mode="OFF", bake_size=32)
        self.assertEqual(source_snapshot(), before)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_outside_target_uv_and_unsupported_final_shader_fail_cleanly(self):
        material, shader = self.material("Invalid bake")
        obj = self.plane(material)
        self.checker(material, shader)
        obj.data.uv_layers.active.data[0].uv = (-.1, 0)
        before = source_snapshot()
        with self.assertRaisesRegex(ExportError, "UV|uv"):
            addon.exporter.export(bpy.context, str(self.directory / "outside.md5mesh"), bake_size=32,
                                  bake_uv_mode="EXISTING")
        self.assertEqual(source_snapshot(), before)
        obj.data.uv_layers.active.data[0].uv = (0, 0)
        mix = material.node_tree.nodes.new("ShaderNodeMixShader")
        material.node_tree.links.new(shader.outputs[0], mix.inputs[1])
        material.node_tree.links.new(mix.outputs[0], material.node_tree.nodes.get("Material Output").inputs["Surface"])
        before = source_snapshot()
        with self.assertRaisesRegex(ExportError, "Principled|Emission|Mix|shader"):
            addon.exporter.export(bpy.context, str(self.directory / "unsupported.md5mesh"), bake_size=32)
        self.assertEqual(source_snapshot(), before)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_forced_failure_after_first_real_bake_cleans_all_temporary_data(self):
        material, shader = self.material("Forced bake failure")
        self.checker(material, shader)
        shader.inputs["Alpha"].default_value = .25
        shader.inputs["Emission Color"].default_value = (1, 0, 0, 1)
        shader.inputs["Emission Strength"].default_value = 1
        self.plane(material)
        before = source_snapshot()
        real_bake = baking.BakeSession._bake_image
        calls = []

        def fail_second(session, *args, **kwargs):
            calls.append(True)
            if len(calls) == 2:
                raise RuntimeError("injected material bake failure")
            return real_bake(session, *args, **kwargs)

        with mock.patch.object(baking.BakeSession, "_bake_image", new=fail_second):
            with self.assertRaisesRegex((RuntimeError, ExportError), "injected material bake failure"):
                addon.exporter.export(bpy.context, str(self.directory / "failed.md5mesh"),
                                      bake_size=32, bake_margin=2, bake_samples=1)
        self.assertEqual(len(calls), 2)
        self.assertEqual(source_snapshot(), before)
        self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BakeIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
