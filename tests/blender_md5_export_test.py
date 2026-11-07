# SPDX-License-Identifier: GPL-2.0-or-later
"""Procedural Blender integration checks; no external assets required.

Run from the repository root:
    blender --background --factory-startup --python-exit-code 1 \
        --python tests/blender_md5_export_test.py
"""

import math
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile
import unittest
from unittest import mock

import bpy
from mathutils import Matrix, Quaternion, Vector


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Tools" / "blender"))
import io_export_quiver_md5 as addon
from io_export_quiver_md5 import bake_cache
from io_export_quiver_md5.model import ExportError


class Tokens:
    def __init__(self, text):
        text = re.sub(r"//[^\n]*", "", text)
        self.items = iter(re.findall(r'"[^"\n]*"|[{}()]|[^\s{}()]+', text))

    def pop(self, expected=None):
        token = next(self.items)
        if expected is not None and token != expected:
            raise AssertionError(f"Expected {expected!r}, got {token!r}")
        return token

    def number(self):
        result = float(self.pop())
        if not math.isfinite(result):
            raise AssertionError("Non-finite exported value")
        return result

    def integer(self):
        return int(self.pop())

    def string(self):
        token = self.pop()
        assert token.startswith('"') and token.endswith('"'), token
        return token[1:-1]

    def vector(self, count=3):
        self.pop("(")
        result = tuple(self.number() for _ in range(count))
        self.pop(")")
        return result


def parse_mesh(path):
    """Independent reader, intentionally not the add-on's serialization code."""
    tokens = Tokens(path.read_text(encoding="utf-8"))
    tokens.pop("MD5Version")
    tokens.pop("10")
    tokens.pop("commandline")
    tokens.string()
    tokens.pop("numJoints")
    num_joints = tokens.integer()
    tokens.pop("numMeshes")
    num_surfaces = tokens.integer()
    tokens.pop("joints")
    tokens.pop("{")
    joints = []
    for _ in range(num_joints):
        joints.append((tokens.string(), tokens.integer(), tokens.vector(), tokens.vector()))
    tokens.pop("}")
    surfaces = []
    for _ in range(num_surfaces):
        tokens.pop("mesh")
        tokens.pop("{")
        tokens.pop("shader")
        shader = tokens.string()
        tokens.pop("numverts")
        vertices = []
        for index in range(tokens.integer()):
            tokens.pop("vert")
            assert tokens.integer() == index
            vertices.append((tokens.vector(2), tokens.integer(), tokens.integer()))
        tokens.pop("numtris")
        triangles = []
        for index in range(tokens.integer()):
            tokens.pop("tri")
            assert tokens.integer() == index
            triangles.append(tuple(tokens.integer() for _ in range(3)))
        tokens.pop("numweights")
        weights = []
        for index in range(tokens.integer()):
            tokens.pop("weight")
            assert tokens.integer() == index
            weights.append((tokens.integer(), tokens.number(), tokens.vector()))
        tokens.pop("}")
        surfaces.append(dict(shader=shader, vertices=vertices, triangles=triangles, weights=weights))
    assert list(tokens.items) == [], "Trailing, unparsed mesh tokens"
    return joints, surfaces


def quaternion(xyz):
    return Quaternion((-math.sqrt(max(0.0, 1.0 - sum(x * x for x in xyz))), *xyz))


def reconstruct(joints, surface):
    positions = []
    for uv, first, count in surface["vertices"]:
        assert count > 0
        influences = surface["weights"][first:first + count]
        assert len(influences) == count
        assert abs(sum(weight[1] for weight in influences) - 1.0) < 2e-5
        position = Vector((0.0, 0.0, 0.0))
        for index, bias, local in influences:
            joint = joints[index]
            position += bias * (Vector(joint[2]) + quaternion(joint[3]) @ Vector(local))
        positions.append(position)
    return positions


def assert_bind_animation(test, path, joints, all_positions):
    tokens = Tokens(path.read_text(encoding="utf-8"))
    for expected in ("MD5Version", "10", "commandline"):
        tokens.pop(expected)
    tokens.string()
    tokens.pop("numFrames")
    test.assertEqual(tokens.integer(), 1)
    tokens.pop("numJoints")
    test.assertEqual(tokens.integer(), len(joints))
    tokens.pop("frameRate")
    test.assertGreater(tokens.integer(), 0)
    tokens.pop("numAnimatedComponents")
    test.assertEqual(tokens.integer(), 0)
    tokens.pop("hierarchy")
    tokens.pop("{")
    for name, parent, _, _ in joints:
        test.assertEqual(tokens.string(), name)
        test.assertEqual(tokens.integer(), parent)
        test.assertEqual(tokens.integer(), 0)
        test.assertEqual(tokens.integer(), 0)
    tokens.pop("}")
    tokens.pop("bounds")
    tokens.pop("{")
    lower, upper = tokens.vector(), tokens.vector()
    for position in all_positions:
        for axis in range(3):
            test.assertLessEqual(lower[axis] - 2e-5, position[axis])
            test.assertGreaterEqual(upper[axis] + 2e-5, position[axis])
    tokens.pop("}")
    tokens.pop("baseframe")
    tokens.pop("{")
    world_poses = []
    for index, joint in enumerate(joints):
        local_position, local_orientation = Vector(tokens.vector()), quaternion(tokens.vector())
        if joint[1] >= 0:
            parent_position, parent_orientation = world_poses[joint[1]]
            position = parent_position + parent_orientation @ local_position
            orientation = parent_orientation @ local_orientation
        else:
            position, orientation = local_position, local_orientation
        test.assertLess((position - Vector(joint[2])).length, 2e-4)
        test.assertAlmostEqual(abs(orientation.dot(quaternion(joint[3]))), 1.0, places=5)
        world_poses.append((position, orientation))
    tokens.pop("}")
    for expected in ("frame", "0", "{", "}"):
        tokens.pop(expected)
    test.assertEqual(list(tokens.items), [])


def create_mesh(name, vertices, faces, material, uvs=None):
    data = bpy.data.meshes.new(name)
    data.from_pydata(vertices, [], faces)
    data.update()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    data.materials.append(material)
    layer = data.uv_layers.new(name="UVMap")
    for index, corner in enumerate(layer.data):
        corner.uv = uvs[index] if uvs else ((index % 2), ((index // 2) % 2))
    return obj


def select_only(*objects):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0] if objects else None


def image(name, color):
    img = bpy.data.images.new(name, width=2, height=2, alpha=True)
    img.pixels = list(color) * 4
    img.pack()
    return img


def texture_path(directory, shader, skin=0, suffix=""):
    filename = f"{PurePosixPath(shader).name}_{skin:02d}_00{suffix}.png"
    paths = list(directory.rglob(filename))
    assert len(paths) == 1, f"Expected one {filename}, found {paths}"
    return paths[0]


def isolate_bake_cache(test):
    """Give each export an empty cache: real bakes, and nothing left in the user's."""
    root = tempfile.TemporaryDirectory(prefix="quiver-md5-bake-cache-")
    test.addCleanup(root.cleanup)
    patcher = mock.patch.object(bake_cache, "default_directory",
                                side_effect=lambda: Path(tempfile.mkdtemp(dir=root.name)))
    patcher.start()
    test.addCleanup(patcher.stop)


class ExportIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        addon.register()

    @classmethod
    def tearDownClass(cls):
        addon.unregister()
        assert not hasattr(bpy.types.Material, "quiver_md5")

    def setUp(self):
        if bpy.context.object and bpy.context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        self.temp = tempfile.TemporaryDirectory(prefix="quiver-md5-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        isolate_bake_cache(self)

    def export(self, name="test", **kwargs):
        path = self.directory / f"{name}.md5mesh"
        options = dict(filepath=str(path), selected_only=True, scale=1.0,
                       forward_axis="NEG_Y", export_rig=False,
                       apply_modifiers=True, texture_prefix="progs", copy_textures=True)
        options.update(kwargs)
        self.assertEqual(bpy.ops.export_scene.quiver_md5(**options), {"FINISHED"})
        self.assertTrue(path.exists())
        self.assertTrue(path.with_suffix(".md5anim").exists())
        return path, parse_mesh(path)

    def assert_cancelled(self, name="invalid", **kwargs):
        path = self.directory / f"{name}.md5mesh"
        try:
            status = bpy.ops.export_scene.quiver_md5(filepath=str(path), **kwargs)
        except RuntimeError:
            # Blender raises on an ERROR report even when the operator cancels.
            status = {"CANCELLED"}
        self.assertEqual(status, {"CANCELLED"})
        self.assertFalse(path.exists())
        self.assertFalse(path.with_suffix(".md5anim").exists())
        return path

    def test_triangulation_uv_seams_material_surfaces_and_mirrors(self):
        first = bpy.data.materials.new("Seamed")
        first.diffuse_color = (0.8, 0.1, 0.2, 1.0)
        second = bpy.data.materials.new("Mirrored")
        second.diffuse_color = (0.1, 0.8, 0.2, 1.0)
        coordinates = [(.125, .25), (.75, .25), (.75, .875), (.125, .875),
                       (.2, .3), (.6, .3), (.6, .7), (.2, .7)]
        seam = create_mesh("Seam", [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0),
                                    (4, 0, 0), (4, 2, 0)],
                           [(0, 1, 2, 3), (1, 4, 5, 2)], first, coordinates)
        mirror = create_mesh("Mirror", [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
                             [(0, 1, 2, 3)], second)
        mirror.location = (8, 0, 0)
        mirror.scale.x = -1
        hidden = create_mesh("NotSelected", [(0, 0, 10), (1, 0, 10), (0, 1, 10)],
                             [(0, 1, 2)], first)
        select_only(seam, mirror)
        path, (joints, surfaces) = self.export()
        self.assertEqual(len(joints), 1)
        self.assertEqual(len(surfaces), 2)
        self.assertEqual(sorted(len(s["vertices"]) for s in surfaces), [4, 8])
        self.assertEqual(sum(len(s["triangles"]) for s in surfaces), 6)
        all_positions = []
        for surface in surfaces:
            points = reconstruct(joints, surface)
            all_positions.extend(points)
            self.assertTrue(all(abs(point.z) < 1e-6 for point in points))
            for a, b, c in surface["triangles"]:
                self.assertLess((points[b] - points[a]).cross(points[c] - points[a]).z, 0)
            self.assertTrue(texture_path(self.directory, surface["shader"]).read_bytes().startswith(b"\x89PNG\r\n\x1a\n"))
        exported_uvs = [v[0] for s in surfaces if len(s["vertices"]) == 8 for v in s["vertices"]]
        for u, v in coordinates:
            self.assertTrue(any(abs(s - u) < 1e-6 and abs(t - (1 - v)) < 1e-6
                                for s, t in exported_uvs))
        assert_bind_animation(self, path.with_suffix(".md5anim"), joints, all_positions)
        self.assertEqual(set(bpy.context.selected_objects), {seam, mirror})
        self.assertFalse(hidden.select_get())

    def test_multiple_packed_skins_and_fullbright(self):
        material = bpy.data.materials.new("Skinned")
        settings = material.quiver_md5
        settings.shader_name = "testskin"
        diffuse = image("Packed red", (1, 0, 0, 1))
        glow = image("Packed glow", (0, 0, 1, 1))
        alternate = image("Packed green", (0, 1, 0, 1))
        first = settings.skins.add()
        first.diffuse, first.fullbright = diffuse, glow
        settings.skins.add().diffuse = alternate
        obj = create_mesh("Skins", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        select_only(obj)
        original_paths = [img.filepath for img in (diffuse, glow, alternate)]
        path, (joints, surfaces) = self.export("skins")
        shader = surfaces[0]["shader"]
        for skin, suffix, channel in [(0, "", 0), (0, "_glow", 2), (1, "", 1)]:
            exported = texture_path(self.directory, shader, skin, suffix)
            loaded = bpy.data.images.load(str(exported), check_existing=False)
            try:
                self.assertEqual(tuple(loaded.size), (2, 2))
                self.assertGreater(loaded.pixels[channel], 0.95)
                self.assertLess(loaded.pixels[(channel + 1) % 3], 0.05)
            finally:
                bpy.data.images.remove(loaded)
        self.assertEqual([img.filepath for img in (diffuse, glow, alternate)], original_paths)
        self.assertTrue(all(img.packed_file for img in (diffuse, glow, alternate)))

    def test_unpacked_dirty_generated_image_preserves_pixels_and_source_state(self):
        material = bpy.data.materials.new("Unpacked skin")
        diffuse = bpy.data.images.new("Unpacked generated", width=2, height=2, alpha=True)
        pixels = [1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1]
        diffuse.pixels = pixels
        diffuse.filepath_raw = "//original_unsaved_image.png"
        self.assertEqual(diffuse.source, "GENERATED")
        self.assertIsNone(diffuse.packed_file)
        original = (diffuse.filepath, diffuse.filepath_raw, diffuse.is_dirty, tuple(diffuse.pixels))
        material.quiver_md5.skins.add().diffuse = diffuse
        obj = create_mesh("Unpacked", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        select_only(obj)
        _, (_, surfaces) = self.export("unpacked")
        exported = bpy.data.images.load(str(texture_path(self.directory, surfaces[0]["shader"])), check_existing=False)
        try:
            self.assertEqual(tuple(exported.size), (2, 2))
            for expected, actual in zip(pixels, exported.pixels):
                self.assertAlmostEqual(expected, actual, places=4)
        finally:
            bpy.data.images.remove(exported)
        self.assertIsNone(diffuse.packed_file)
        self.assertEqual((diffuse.filepath, diffuse.filepath_raw, diffuse.is_dirty, tuple(diffuse.pixels)), original)

    def test_principled_base_color_image_detection(self):
        material = bpy.data.materials.new("Node image")
        material.use_nodes = True
        nodes = material.node_tree.nodes
        node = nodes.new("ShaderNodeTexImage")
        node.image = image("Automatic texture", (0, 1, 0, 1))
        material.node_tree.links.new(node.outputs["Color"], nodes.get("Principled BSDF").inputs["Base Color"])
        obj = create_mesh("Nodes", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        select_only(obj)
        _, (_, surfaces) = self.export("nodes")
        exported = bpy.data.images.load(str(texture_path(self.directory, surfaces[0]["shader"])), check_existing=False)
        try:
            self.assertEqual(tuple(exported.size), (2, 2))
            self.assertGreater(exported.pixels[1], .95)
        finally:
            bpy.data.images.remove(exported)

    def test_reexport_removes_owned_obsolete_skins_and_preserves_other_files(self):
        material = bpy.data.materials.new("Changing skins")
        settings = material.quiver_md5
        settings.shader_name = "before"
        diffuse = image("Cleanup diffuse", (1, 0, 0, 1))
        first = settings.skins.add()
        first.diffuse = diffuse
        first.fullbright = image("Cleanup glow", (0, 0, 1, 1))
        settings.skins.add().diffuse = image("Cleanup alternate", (0, 1, 0, 1))
        obj = create_mesh("Changing skins", [(0, 0, 0), (1, 0, 0), (0, 1, 0)],
                          [(0, 1, 2)], material)
        select_only(obj)
        unrelated = self.directory / "artist_reference.png"
        unrelated.write_bytes(b"unrelated original contents")
        _, (_, surfaces) = self.export("cleanup")
        shader = surfaces[0]["shader"]
        previous_diffuse = texture_path(self.directory, shader)
        previous_glow = texture_path(self.directory, shader, suffix="_glow")
        previous_alternate = texture_path(self.directory, shader, skin=1)
        settings.skins.remove(1)
        settings.skins[0].fullbright = None
        self.export("cleanup")
        self.assertTrue(previous_diffuse.is_file())
        self.assertFalse(previous_glow.exists())
        self.assertFalse(previous_alternate.exists())
        settings.shader_name = "after"
        _, (_, renamed) = self.export("cleanup")
        self.assertFalse(previous_diffuse.exists())
        self.assertTrue(texture_path(self.directory, renamed[0]["shader"]).is_file())
        self.assertEqual(unrelated.read_bytes(), b"unrelated original contents")

    def test_other_model_cannot_overwrite_owned_texture_stem(self):
        material = bpy.data.materials.new("Shared stem")
        material.quiver_md5.shader_name = "shared_skin"
        material.quiver_md5.skins.add().diffuse = image("Owned original", (1, 0, 0, 1))
        obj = create_mesh("Ownership", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        select_only(obj)
        self.export("owner_a")
        before = {path.name: path.read_bytes() for path in self.directory.iterdir()}
        material.quiver_md5.skins[0].diffuse = image("Attempted replacement", (0, 1, 0, 1))
        with self.assertRaises(ExportError):
            addon.exporter.export(bpy.context, str(self.directory / "owner_b.md5mesh"))
        self.assertTrue(all(path.is_file() for path in self.directory.iterdir()))
        self.assertEqual({path.name: path.read_bytes() for path in self.directory.iterdir()}, before)

    def test_publish_failure_rolls_back_original_bundle_and_manifest(self):
        material = bpy.data.materials.new("Transactional skin")
        settings = material.quiver_md5
        settings.shader_name = "transaction"
        first = settings.skins.add()
        first.diffuse = image("Original transaction skin", (1, 0, 0, 1))
        first.fullbright = image("Original transaction glow", (0, 0, 1, 1))
        settings.skins.add().diffuse = image("Original transaction alternate", (0, 1, 0, 1))
        obj = create_mesh("Transaction", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        select_only(obj)
        path, _ = self.export("transaction")
        manifest_name = f".{path.name}.quiver.json"
        self.assertTrue((self.directory / manifest_name).is_file())
        before = {file.name: file.read_bytes() for file in self.directory.iterdir()}
        settings.skins.remove(1)
        settings.skins[0].diffuse = image("New transaction skin", (0, 1, 0, 1))
        settings.skins[0].fullbright = None
        obj.data.vertices[0].co.x = -.5
        obj.data.update()
        real_replace = addon.exporter.os.replace
        state = {"failed": False, "backups": 0}

        def fail_manifest_install_once(source, destination):
            source, destination = Path(source), Path(destination)
            if destination.name.startswith("backup_") and destination.parent.name.startswith(".quiver-md5-"):
                state["backups"] += 1
            if (not state["failed"] and source.name == manifest_name and
                    source.parent.name.startswith(".quiver-md5-")):
                state["failed"] = True
                raise OSError("injected publication failure")
            return real_replace(source, destination)

        with mock.patch.object(addon.exporter.os, "replace", side_effect=fail_manifest_install_once):
            with self.assertRaisesRegex(OSError, "injected publication failure"):
                addon.exporter.export(bpy.context, str(path))
        self.assertTrue(state["failed"], "The failure injection must be reached")
        self.assertGreater(state["backups"], 1, "Failure must follow moves of existing output files")
        self.assertTrue(all(file.is_file() for file in self.directory.iterdir()), "Staging directory was not removed")
        self.assertEqual({file.name: file.read_bytes() for file in self.directory.iterdir()}, before)

    def test_render_uv_is_exported_and_different_explicit_uv_is_rejected(self):
        material = bpy.data.materials.new("Render UV")
        material.use_nodes = True
        nodes = material.node_tree.nodes
        node = nodes.new("ShaderNodeTexImage")
        node.image = image("Render UV texture", (1, 0, 0, 1))
        material.node_tree.links.new(node.outputs["Color"], nodes.get("Principled BSDF").inputs["Base Color"])
        obj = create_mesh("Two UV maps", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material,
                          [(.1, .2), (.3, .4), (.5, .6)])
        edit_uv = obj.data.uv_layers[0]
        edit_uv.name = "EditUV"
        render_uv = obj.data.uv_layers.new(name="RenderUV")
        coordinates = [(.71, .13), (.89, .27), (.62, .94)]
        for corner, value in zip(render_uv.data, coordinates):
            corner.uv = value
        obj.data.uv_layers.active_index = 0
        render_uv.active_render = True
        self.assertEqual(obj.data.uv_layers.active.name, "EditUV")
        self.assertTrue(render_uv.active_render)
        select_only(obj)
        _, (_, surfaces) = self.export("render_uv")
        exported_uvs = [vertex[0] for surface in surfaces for vertex in surface["vertices"]]
        for u, v in coordinates:
            self.assertTrue(any(abs(s - u) < 1e-6 and abs(t - (1 - v)) < 1e-6
                                for s, t in exported_uvs))
        uv_node = nodes.new("ShaderNodeUVMap")
        uv_node.uv_map = "EditUV"
        material.node_tree.links.new(uv_node.outputs["UV"], node.inputs["Vector"])
        self.assert_cancelled("wrong_explicit_uv", bake_mode="OFF")
        self.assertEqual(obj.data.uv_layers.active.name, "EditUV")
        self.assertTrue(render_uv.active_render)

    def test_armature_rest_pose_reconstruction_and_scene_restoration(self):
        rig_data = bpy.data.armatures.new("Rig")
        rig = bpy.data.objects.new("Rig", rig_data)
        bpy.context.collection.objects.link(rig)
        select_only(rig)
        bpy.ops.object.mode_set(mode="EDIT")
        root = rig_data.edit_bones.new("root")
        root.head, root.tail = (0, 0, 0), (0, 0, 1)
        child = rig_data.edit_bones.new("child")
        child.head, child.tail = (0, 0, 1), (1, 0, 1)
        child.parent = root
        bpy.ops.object.mode_set(mode="OBJECT")
        transform = Matrix.Translation((3, -2, 5)) @ Matrix.Rotation(.4, 4, "Z")
        rig.matrix_world = transform
        material = bpy.data.materials.new("Rig skin")
        vertices = [(0, 0, .25), (.5, 0, 1), (.2, .4, .8)]
        obj = create_mesh("Weighted", vertices, [(0, 1, 2)], material)
        obj.matrix_world = transform
        modifier = obj.modifiers.new("Skin", "ARMATURE")
        modifier.object = rig
        obj.vertex_groups.new(name="root").add([0, 2], .75, "REPLACE")
        obj.vertex_groups.new(name="child").add([1, 2], .25, "REPLACE")
        # Deliberately pose the skeleton: export_rig must use the rest geometry.
        rig.pose.bones["child"].rotation_mode = "XYZ"
        rig.pose.bones["child"].rotation_euler.y = .7
        rig_data.pose_position = "POSE"
        bpy.context.scene.frame_set(7)
        select_only(obj)
        original_pose = rig.pose.bones["child"].matrix_basis.copy()
        path, (joints, surfaces) = self.export("rigged", export_rig=True)
        self.assertEqual([joint[0] for joint in joints], ["root", "child"])
        self.assertEqual([joint[1] for joint in joints], [-1, 0])
        points = reconstruct(joints, surfaces[0])
        expected = []
        for vertex in vertices:
            point = transform @ Vector(vertex)
            expected.append(Vector((-point.y, point.x, point.z)))
        for point in points:
            self.assertLess(min((point - target).length for target in expected), 2e-4)
        self.assertEqual(sorted(v[2] for v in surfaces[0]["vertices"]), [1, 1, 2])
        assert_bind_animation(self, path.with_suffix(".md5anim"), joints, points)
        self.assertEqual(rig_data.pose_position, "POSE")
        self.assertTrue(modifier.show_viewport)
        self.assertEqual(bpy.context.scene.frame_current, 7)
        self.assertEqual(rig.pose.bones["child"].matrix_basis, original_pose)
        self.assertEqual(set(bpy.context.selected_objects), {obj})

    def test_invalid_input_cancels_before_writing(self):
        material = bpy.data.materials.new("No UV")
        obj = create_mesh("No UV", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        obj.data.uv_layers.remove(obj.data.uv_layers.active)
        select_only(obj)
        self.assert_cancelled(bake_uv_mode="EXISTING")
        self.assertEqual(set(bpy.context.selected_objects), {obj})

    def test_rig_failure_restores_enabled_and_disabled_modifiers(self):
        rig_data = bpy.data.armatures.new("Failure Rig")
        rig = bpy.data.objects.new("Failure Rig", rig_data)
        bpy.context.collection.objects.link(rig)
        select_only(rig)
        bpy.ops.object.mode_set(mode="EDIT")
        bone = rig_data.edit_bones.new("root")
        bone.head, bone.tail = (0, 0, 0), (0, 0, 1)
        bpy.ops.object.mode_set(mode="OBJECT")
        material = bpy.data.materials.new("Unweighted skin")
        obj = create_mesh("Unweighted", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], material)
        active = obj.modifiers.new("Enabled skin", "ARMATURE")
        active.object = rig
        inactive = obj.modifiers.new("Disabled skin", "ARMATURE")
        inactive.object = rig
        inactive.show_viewport = False
        obj.vertex_groups.new(name="root").add([0, 1], 1, "REPLACE")
        rig.pose.bones["root"].rotation_mode = "XYZ"
        rig.pose.bones["root"].rotation_euler.x = .4
        rig_data.pose_position = "POSE"
        bpy.context.scene.frame_set(11)
        original_pose = rig.pose.bones["root"].matrix_basis.copy()
        select_only(obj)
        self.assert_cancelled("unweighted", export_rig=True)
        self.assertTrue(active.show_viewport)
        self.assertFalse(inactive.show_viewport)
        self.assertEqual(rig_data.pose_position, "POSE")
        self.assertEqual(rig.pose.bones["root"].matrix_basis, original_pose)
        self.assertEqual(bpy.context.scene.frame_current, 11)
        self.assertEqual(set(bpy.context.selected_objects), {obj})
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_mismatched_surface_skin_counts_fail_before_writing(self):
        first = bpy.data.materials.new("Two skins")
        first.quiver_md5.skins.add().diffuse = image("Mismatch zero", (1, 0, 0, 1))
        first.quiver_md5.skins.add().diffuse = image("Mismatch one", (0, 1, 0, 1))
        second = bpy.data.materials.new("One skin")
        left = create_mesh("Left", [(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)], first)
        right = create_mesh("Right", [(2, 0, 0), (3, 0, 0), (2, 1, 0)], [(0, 1, 2)], second)
        select_only(left, right)
        self.assert_cancelled("mismatched_skins")
        self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ExportIntegrationTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
