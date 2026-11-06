#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure MD5 backend tests; no Blender installation is needed.

Run with: python3 -m unittest discover -s tests -p test_blender_md5_format.py
"""

from dataclasses import replace
import importlib.util
import math
from pathlib import Path
import re
import sys
import types
import unittest


# Load the backend without executing the Blender add-on's bpy-dependent init.
ROOT = Path(__file__).resolve().parents[1]
PACKAGE_PATH = ROOT / "Tools/blender/io_export_quiver_md5"
PACKAGE_NAME = "quiver_md5_format_test"
package = types.ModuleType(PACKAGE_NAME)
package.__path__ = [str(PACKAGE_PATH)]
sys.modules[PACKAGE_NAME] = package


def _module(name):
    spec = importlib.util.spec_from_file_location(f"{PACKAGE_NAME}.{name}", PACKAGE_PATH / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


model = _module("model")
md5 = _module("md5")


def fixture():
    # Absolute skeleton: parent rotates +90 degrees about Z, child +180.
    half = math.sqrt(0.5)
    joints = (model.Joint("root", -1, (2.0, 3.0, 4.0), (0.0, 0.0, -half)),
              model.Joint("child", 0, (2.0, 5.0, 4.0), (0.0, 0.0, -1.0)))
    vertices = (model.Vertex((0.0, 0.0), (model.Weight(1, 1.0, (1.0, 0.0, 0.0)),)),
                model.Vertex((1.0, 0.0), (model.Weight(0, 1.0, (0.0, 0.0, 1.0)),)),
                model.Vertex((0.5, 1.0), (model.Weight(0, 0.25, (0.0, 0.0, 0.0)),
                                                 model.Weight(1, 0.75, (0.0, 0.0, 0.0)))))
    return model.Model(joints, (model.Surface("body", "progs/test_body", vertices, ((0, 2, 1),)),))


def vectors(text):
    return [tuple(float(number) for number in group.split())
            for group in re.findall(r"\( ([^()]*) \)", text)]


class MD5FormatTest(unittest.TestCase):
    def test_mesh_uses_flat_weight_offsets_and_preserves_backend_coordinates(self):
        text = md5.mesh_text(fixture())
        self.assertTrue(text.startswith("MD5Version 10\n"))
        self.assertIn('shader "progs/test_body"', text)
        self.assertIn("numJoints 2", text)
        self.assertIn("numMeshes 1", text)
        self.assertIn("vert 0 ( 0 0 ) 0 1", text)
        self.assertIn("vert 1 ( 1 0 ) 1 1", text)
        self.assertIn("vert 2 ( 0.5 1 ) 2 2", text)
        self.assertIn("numweights 4", text)
        self.assertIn("tri 0 0 2 1", text)
        self.assertIn("weight 3 1 0.75 ( 0 0 0 )", text)
        self.assertEqual(text, md5.mesh_text(fixture()))

    def test_bind_animation_converts_absolute_joints_to_parent_local(self):
        text = md5.bind_animation_text(fixture())
        self.assertIn("numFrames 1", text)
        self.assertIn("numAnimatedComponents 0", text)
        self.assertIn('"root" -1 0 0', text)
        self.assertIn('"child" 0 0 0', text)
        base = text.split("baseframe {", 1)[1].split("}", 1)[0]
        root_position, root_orientation, child_position, child_orientation = vectors(base)
        self.assertEqual(root_position, (2.0, 3.0, 4.0))
        self.assertAlmostEqual(root_orientation[2], -math.sqrt(0.5), places=8)
        for actual, expected in zip(child_position, (2.0, 0.0, 0.0)):
            self.assertAlmostEqual(actual, expected, places=7)
        for actual, expected in zip(child_orientation, (0.0, 0.0, -math.sqrt(0.5))):
            self.assertAlmostEqual(actual, expected, places=8)
        self.assertRegex(text, r"frame 0 \{\s*\}")

    def test_bounds_reconstruct_rotated_and_blended_joint_local_weights(self):
        text = md5.bind_animation_text(fixture())
        bounds = text.split("bounds {", 1)[1].split("}", 1)[0]
        low, high = vectors(bounds)
        for actual, expected in zip(low, (1.0, 3.0, 4.0)):
            self.assertAlmostEqual(actual, expected, places=7)
        for actual, expected in zip(high, (2.0, 5.0, 5.0)):
            self.assertAlmostEqual(actual, expected, places=7)

    def test_multiple_surfaces_have_independent_weight_indices(self):
        asset = fixture()
        second = replace(asset.surfaces[0], name="head", shader="progs/test_head")
        text = md5.mesh_text(replace(asset, surfaces=(*asset.surfaces, second)))
        self.assertIn("numMeshes 2", text)
        self.assertEqual(text.count("vert 0 ( 0 0 ) 0 1"), 2)
        self.assertEqual(text.count("weight 0 1 1"), 2)

    def test_invalid_assets_are_rejected_by_both_writers(self):
        asset = fixture()
        surface = asset.surfaces[0]
        vertex = surface.vertices[0]
        bad_surfaces = [
            replace(surface, shader="../outside"),
            replace(surface, shader="/absolute"),
            replace(surface, shader="progs//body"),
            replace(surface, shader="progs/body.png"),
            replace(surface, shader="x" * 49),
            replace(surface, name="body\ninjected"),
            replace(surface, vertices=()),
            replace(surface, vertices=(vertex,) * 65536),
            replace(surface, triangles=()),
            replace(surface, triangles=((0, 0, 1),)),
            replace(surface, triangles=((0, 1, 3),)),
            replace(surface, triangles=((0, 1, -1),)),
        ]
        bad_vertices = [
            replace(vertex, uv=(float("nan"), 0.0)),
            replace(vertex, weights=()),
            replace(vertex, weights=(model.Weight(0, 0.5, (0.0, 0.0, 0.0)),)),
            replace(vertex, weights=(model.Weight(0, 0.0, (0.0, 0.0, 0.0)),)),
            replace(vertex, weights=(model.Weight(2, 1.0, (0.0, 0.0, 0.0)),)),
            replace(vertex, weights=(model.Weight(0, 1.0, (float("inf"), 0.0, 0.0)),)),
            replace(vertex, weights=(model.Weight(0, 0.5, (0.0, 0.0, 0.0)),) * 2),
            replace(vertex, weights=(model.Weight(0, 0.2, (0.0, 0.0, 0.0)),) * 5),
        ]
        bad_joints = [
            replace(asset.joints[0], name='root"'),
            replace(asset.joints[0], name="x" * 32),
            replace(asset.joints[0], parent=0),
            replace(asset.joints[0], orientation=(2.0, 0.0, 0.0)),
            replace(asset.joints[0], position=(0.0, float("inf"), 0.0)),
        ]
        assets = [replace(asset, joints=()), replace(asset, joints=(asset.joints[0],) * 257),
                  replace(asset, surfaces=()), replace(asset, joints=(asset.joints[0], replace(asset.joints[1], name="root")))]
        assets.extend(replace(asset, surfaces=(s,)) for s in bad_surfaces)
        assets.extend(replace(asset, surfaces=(replace(surface, vertices=(v, *surface.vertices[1:])),)) for v in bad_vertices)
        assets.extend(replace(asset, joints=(j, asset.joints[1])) for j in bad_joints)
        for i, invalid in enumerate(assets):
            for writer in (md5.mesh_text, md5.bind_animation_text):
                with self.subTest(case=i, writer=writer.__name__), self.assertRaises(model.ExportError):
                    writer(invalid)

    def test_frame_rate_is_validated(self):
        for value in (0, -1, 1.5, float("inf"), True):
            with self.subTest(value=value), self.assertRaises(model.ExportError):
                md5.bind_animation_text(fixture(), value)

    def test_surface_chunking_preserves_geometry_material_and_weights(self):
        asset = fixture()
        original = asset.surfaces[0]
        vertices = tuple(replace(original.vertices[0], uv=(i / 5, i / 7)) for i in range(6))
        source = replace(original, vertices=vertices, triangles=((0, 1, 2), (2, 3, 4), (4, 5, 0)))
        chunks = model.split_surface(source, max_vertices=4)
        self.assertEqual(len(chunks), 3)
        self.assertTrue(all(len(chunk.vertices) <= 4 and chunk.shader == source.shader for chunk in chunks))
        expected = [tuple(source.vertices[i] for i in triangle) for triangle in source.triangles]
        actual = [tuple(chunk.vertices[i] for i in triangle) for chunk in chunks for triangle in chunk.triangles]
        self.assertEqual(actual, expected)
        self.assertIs(model.split_surface(source, max_vertices=6)[0], source)
        self.assertIn('numMeshes 3', md5.mesh_text(replace(asset, surfaces=chunks)))

    def test_surface_chunking_obeys_actual_uint16_export_limit(self):
        asset = fixture()
        source = replace(asset.surfaces[0], vertices=(asset.surfaces[0].vertices[0],) * 65536,
                         triangles=tuple((i, i + 1, i + 2) for i in range(0, 65535, 3)) + ((65533, 65534, 65535),))
        chunks = model.split_surface(source)
        self.assertEqual([len(chunk.vertices) for chunk in chunks], [65535, 3])
        self.assertEqual(sum(len(chunk.triangles) for chunk in chunks), len(source.triangles))
        model.validate_model(replace(asset, surfaces=chunks))


if __name__ == "__main__":
    unittest.main()
