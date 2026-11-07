#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bake sizing and disk cache tests; no Blender installation is needed.

Run with: python3 -m unittest discover -s tests -p test_blender_md5_bake_planning.py
"""

import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_PATH = ROOT / "Tools/blender/io_export_quiver_md5"
PACKAGE_NAME = "quiver_md5_planning_test"
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
sizing = _module("sizing")
bake_cache = _module("bake_cache")
KEY_A, KEY_B, KEY_C = "a" * 64, "b" * 64, "c" * 64


class SizingTests(unittest.TestCase):
    def size(self, area, coverage=1.0, **policy):
        return sizing.choose_size(sizing.SizePolicy(**policy), area, coverage).size

    def test_size_scales_with_the_square_root_of_area(self):
        options = dict(density=1, minimum=32, maximum=4096)
        self.assertEqual(self.size(128 ** 2, **options), 128)
        self.assertEqual(self.size(256 ** 2, **options), 256)
        self.assertEqual(self.size(1024 ** 2, **options), 1024)

    def test_size_scales_linearly_with_density(self):
        self.assertEqual(self.size(100 ** 2, density=1.28, maximum=4096), 128)
        self.assertEqual(self.size(100 ** 2, density=5.12, maximum=4096), 512)

    def test_partial_uv_coverage_spends_more_pixels(self):
        full = sizing.choose_size(sizing.SizePolicy(density=1, maximum=4096), 256 ** 2, 1.0)
        quarter = sizing.choose_size(sizing.SizePolicy(density=1, maximum=4096), 256 ** 2, .25)
        self.assertAlmostEqual(quarter.ideal, full.ideal * 2)
        self.assertEqual(quarter.size, full.size * 2)

    def test_coverage_above_one_tile_is_capped(self):
        self.assertEqual(self.size(128 ** 2, 3.0, density=1), 128)

    def test_rounding_is_nearest_in_log_space(self):
        options = dict(density=1, minimum=32, maximum=4096)
        self.assertEqual(self.size(180 ** 2, **options), 128)  # Below 128·√2.
        self.assertEqual(self.size(182 ** 2, **options), 256)  # Above 128·√2.

    def test_density_stays_within_root_two_between_limits(self):
        policy = sizing.SizePolicy(density=4, minimum=32, maximum=4096)
        for step in range(1, 400):
            area = 1.05 ** step
            for coverage in (.2, .6, 1):
                choice = sizing.choose_size(policy, area, coverage)
                if choice.limit is None:
                    ratio = sizing.achieved_density(choice.size, area, coverage) / 4
                    self.assertLessEqual(max(ratio, 1 / ratio), math.sqrt(2) * 1.0001)

    def test_limits_clamp_and_report(self):
        policy = sizing.SizePolicy(density=4, minimum=64, maximum=256)
        tiny = sizing.choose_size(policy, 1, 1)
        huge = sizing.choose_size(policy, 1e8, 1)
        self.assertEqual((tiny.size, tiny.limit), (64, "MIN"))
        self.assertEqual((huge.size, huge.limit), (256, "MAX"))

    def test_automatic_sizes_stay_powers_of_two_inside_odd_limits(self):
        policy = sizing.SizePolicy(density=4, minimum=100, maximum=3000)
        self.assertEqual(sizing.choose_size(policy, 1, 1).size, 128)
        self.assertEqual(sizing.choose_size(policy, 1e9, 1).size, 2048)

    def test_limits_without_a_power_of_two_between_them_stay_near(self):
        policy = sizing.SizePolicy(density=4, minimum=100, maximum=120)
        self.assertEqual(sizing.choose_size(policy, 100, 1).size, 64)

    def test_empty_surface_gets_the_minimum(self):
        self.assertEqual(self.size(0), 64)
        self.assertEqual(self.size(10, 0), 64)

    def test_fixed_mode_ignores_area_and_keeps_any_size(self):
        choice = sizing.choose_size(sizing.SizePolicy(mode="FIXED", fixed_size=300), 1e9, .1)
        self.assertEqual((choice.size, choice.source), (300, "FIXED"))

    def test_material_override_wins_in_every_mode(self):
        for mode in ("AUTO", "FIXED"):
            choice = sizing.choose_size(sizing.SizePolicy(mode=mode), 1e9, 1, override=96)
            self.assertEqual((choice.size, choice.source), (96, "MATERIAL"))

    def test_invalid_settings_are_rejected(self):
        for kwargs in (dict(mode="BIG"), dict(density=0), dict(density=math.inf), dict(minimum=16),
                       dict(maximum=8192), dict(minimum=512, maximum=256), dict(fixed_size=True)):
            with self.subTest(kwargs), self.assertRaises(model.ExportError):
                sizing.SizePolicy(**kwargs)
        with self.assertRaises(model.ExportError):
            sizing.choose_size(sizing.SizePolicy(), 10, 1, override=8)
        with self.assertRaises(model.ExportError):
            sizing.choose_size(sizing.SizePolicy(), math.nan, 1)


class SettleSizeTests(unittest.TestCase):
    """Generated layouts: fixed-pixel margins make coverage rise with size."""

    policy = sizing.SizePolicy(density=4, minimum=64, maximum=2048)

    def settle(self, area, coverage, policy=None):
        calls = []

        def coverage_at(size):
            calls.append(size)
            return coverage(size)

        return sizing.settle_size(policy or self.policy, area, coverage_at), calls

    def test_constant_coverage_matches_choose_size(self):
        for step in range(1, 120):
            area = 1.2 ** step
            for coverage in (.05, .3, .9):
                settled, _ = self.settle(area, lambda size: coverage)
                self.assertEqual(settled.size, sizing.choose_size(self.policy, area, coverage).size, (area, coverage))

    def test_result_is_the_smallest_size_that_reaches_the_target(self):
        def coverage(size):  # Shaped like the measured Sponza curves.
            return min(.8, 2e-7 * size ** 1.6 + .001)

        for step in range(1, 90):
            area = 1.2 ** step
            choice, _ = self.settle(area, coverage)
            if choice.limit is None and choice.size > 64:
                below = choice.size // 2
                self.assertLess(sizing.achieved_density(below, area, coverage(below)) * math.sqrt(2), 4)
                self.assertGreaterEqual(sizing.achieved_density(choice.size, area, coverage(choice.size)) * math.sqrt(2), 4)

    def test_projections_stay_few(self):
        def coverage(size):
            return min(.8, 2e-7 * size ** 1.6 + .001)

        counts = [len(set(self.settle(1.2 ** step, coverage)[1])) for step in range(1, 90)]
        self.assertLess(max(counts), 6)  # Fewer than the six sizes from 64 to 2048.
        self.assertLess(sum(counts) / len(counts), 3)

    def test_unreachable_target_stops_at_max_and_reports(self):
        choice, _ = self.settle(1e7, lambda size: .01)
        self.assertEqual((choice.size, choice.limit), (2048, "MAX"))

    def test_tiny_surface_stops_at_min(self):
        choice, _ = self.settle(1, lambda size: .5)
        self.assertEqual((choice.size, choice.limit), (64, "MIN"))

    def test_zero_coverage_and_area_terminate(self):
        self.assertEqual(self.settle(100, lambda size: 0)[0].size, 2048)
        self.assertEqual(self.settle(0, lambda size: 1 / 0)[0].size, 64)

    def test_every_bracket_shape_terminates(self):
        for minimum, maximum in ((32, 32), (64, 64), (32, 4096), (100, 120)):
            policy = sizing.SizePolicy(density=4, minimum=minimum, maximum=maximum)
            for area in (1e-3, 1, 1e3, 1e6, 1e9):
                choice, _ = self.settle(area, lambda size: .2, policy)
                self.assertGreater(choice.size, 0)


class KeyTests(unittest.TestCase):
    def test_key_ignores_dictionary_order_and_tracks_values(self):
        self.assertEqual(bake_cache.make_key({"a": 1, "b": [2]}), bake_cache.make_key({"b": [2], "a": 1}))
        self.assertNotEqual(bake_cache.make_key({"a": 1}), bake_cache.make_key({"a": 2}))

    def test_default_directory_follows_the_platform_cache(self):
        with mock.patch.object(sys, "platform", "linux"), mock.patch.dict(os.environ, {"XDG_CACHE_HOME": "/x/cache"}):
            self.assertEqual(bake_cache.default_directory(), Path("/x/cache/quiver_md5/bakes"))


class CacheTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="quiver-md5-cache-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "cache"
        self.source = Path(temp.name) / "source"
        self.source.mkdir()
        self.cache = bake_cache.BakeCache(self.root)

    def files(self, text="pixels", **extra):
        paths = {}
        for name, value in {"diffuse.png": text, **extra}.items():
            paths[name] = self.source / name
            paths[name].write_text(value)
        return paths

    def stored(self, key=KEY_A, text="pixels"):
        return self.cache.put(key, self.files(text), {"size": 64})

    def test_round_trip_survives_a_new_cache_object(self):
        self.cache.put(KEY_A, self.files(**{"glow.png": "glow"}), {"size": 64})
        entry = bake_cache.BakeCache(self.root).lookup(KEY_A)
        self.assertEqual(entry.path("diffuse.png").read_text(), "pixels")
        self.assertEqual(entry.path("glow.png").read_text(), "glow")
        self.assertIsNone(entry.path("uvs.bin"))
        self.assertEqual(entry.meta, {"size": 64})

    def test_missing_key_is_a_quiet_miss(self):
        self.assertIsNone(self.cache.lookup(KEY_B))
        self.assertEqual(self.cache.problems, [])

    def test_damage_is_detected_reported_and_removed(self):
        def flip(entry):
            data = bytearray(entry.path("diffuse.png").read_bytes())
            data[0] ^= 1
            entry.path("diffuse.png").write_bytes(bytes(data))

        def rewrite_manifest(field, value):
            def change(entry):
                path = entry.directory / bake_cache.MANIFEST
                data = json.loads(path.read_text())
                data[field] = value
                path.write_text(json.dumps(data))
            return change

        damages = {
            "same-length edit": flip,
            "truncated": lambda entry: entry.path("diffuse.png").write_bytes(b"pix"),
            "deleted file": lambda entry: entry.path("diffuse.png").unlink(),
            "deleted manifest": lambda entry: (entry.directory / bake_cache.MANIFEST).unlink(),
            "bad JSON": lambda entry: (entry.directory / bake_cache.MANIFEST).write_text("{"),
            "wrong key": rewrite_manifest("key", KEY_B),
            "old format": rewrite_manifest("format", 0),
            "escaping name": rewrite_manifest("files", {"../diffuse.png": {"size": 6, "digest": "x"}}),
        }
        for label, damage in damages.items():
            with self.subTest(label):
                self.cache.problems.clear()
                entry = self.stored()
                damage(entry)
                self.assertIsNone(self.cache.lookup(KEY_A))
                self.assertEqual(len(self.cache.problems), 1)
                self.assertFalse(entry.directory.exists())

    def test_put_replaces_a_forced_rebake(self):
        self.stored(text="old")
        self.stored(text="new")
        self.assertEqual(self.cache.lookup(KEY_A).path("diffuse.png").read_text(), "new")
        self.assertEqual(self.cache.usage()[0], 1)

    def test_failed_put_leaves_nothing_visible(self):
        self.stored(text="old")
        with mock.patch.object(bake_cache.shutil, "copyfile", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.stored(KEY_B)
        self.assertIsNone(self.cache.lookup(KEY_B))
        self.assertEqual(self.cache.problems, [])
        self.assertEqual(list((self.root / f"v{bake_cache.FORMAT}").glob(".tmp-*")), [])

    def test_losing_a_publish_race_keeps_the_winner(self):
        winner = self.stored(text="winner").directory
        loser = self.files("loser")

        def race(source, target):
            # The other process publishes between our removal and our rename.
            target.mkdir()
            for path in winner_files:
                (target / path.name).write_bytes(path.read_bytes())
            raise FileExistsError("another Blender published first")

        winner_copy = winner.parent / "winner-copy"
        winner.rename(winner_copy)
        winner_files = list(winner_copy.iterdir())
        with mock.patch.object(bake_cache.os, "rename", side_effect=race):
            entry = self.cache.put(KEY_A, loser, {"size": 64})
        self.assertEqual(entry.path("diffuse.png").read_text(), "winner")

    def test_eviction_removes_least_recently_used_and_spares_keep(self):
        for index, key in enumerate((KEY_A, KEY_B, KEY_C)):
            entry = self.stored(key, "x" * 1000)
            os.utime(entry.directory / bake_cache.MANIFEST, (1000 + index, 1000 + index))
        per_entry = self.cache.usage()[1] // 3
        self.cache.limit = per_entry
        self.assertEqual(self.cache.evict(keep={KEY_A}), 2)
        self.assertIsNotNone(self.cache.lookup(KEY_A))
        self.assertIsNone(self.cache.lookup(KEY_B))
        self.assertIsNone(self.cache.lookup(KEY_C))

    def test_lookup_refreshes_recency(self):
        for index, key in enumerate((KEY_A, KEY_B)):
            entry = self.stored(key, "x" * 1000)
            os.utime(entry.directory / bake_cache.MANIFEST, (1000 + index, 1000 + index))
        self.cache.lookup(KEY_A)
        self.cache.limit = self.cache.usage()[1] // 2
        self.cache.evict()
        self.assertIsNotNone(self.cache.lookup(KEY_A))
        self.assertIsNone(self.cache.lookup(KEY_B))

    def test_abandoned_staging_is_swept_only_when_old(self):
        self.stored()
        base = self.root / f"v{bake_cache.FORMAT}"
        fresh, old = base / ".tmp-fresh", base / ".tmp-old"
        fresh.mkdir()
        old.mkdir()
        os.utime(old, (1000, 1000))
        self.cache.evict()
        self.assertTrue(fresh.exists())
        self.assertFalse(old.exists())

    def test_clear_touches_only_cache_entries(self):
        self.stored()
        bystander = self.root / "notes.txt"
        odd_directory = self.root / f"v{bake_cache.FORMAT}" / "aa" / "not-a-key"
        odd_directory.mkdir(parents=True)
        bystander.write_text("mine")
        self.assertEqual(self.cache.clear(), 1)
        self.assertEqual(self.cache.usage(), (0, 0))
        self.assertTrue(bystander.exists())
        self.assertTrue(odd_directory.exists())

    def test_invalid_keys_cannot_escape_the_root(self):
        with self.assertRaises(ValueError):
            self.cache.lookup("../" * 3 + "x")


if __name__ == "__main__":
    unittest.main()
