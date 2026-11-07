#!/usr/bin/env python3
"""Validate benchmark interpretation without starting a game or using a GPU."""
import unittest
import argparse
import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import bench_radiance as rc


class RadianceBenchmarkTest(unittest.TestCase):
    def test_full_texture_and_model_submission_guards_are_explicit(self):
        state = ('reduced receiver requested=0 active=0 checkerboard requested=1 active=0\n'
                 'receiver scale requested=1 active=1 divisor=1\n'
                 'receiver filter requested=texture active=texture\n'
                 'deferred opaque requested=1 models_active=1 draws=25 triangles=250000 frames_active=4 frames_fallback=7')
        rc.verify_receiver_state(state, 'full', 1, 'texture', True,
                                 full_texture=True, checkerboard_requested=True)
        # Existing/historical full checks still require active=full by default.
        with self.assertRaises(RuntimeError):
            rc.verify_receiver_state(state, 'full', 1, 'texture', True, checkerboard_requested=True)
        for bad in (state.replace('active=texture', 'active=full'),
                    state.replace('active=1 divisor=1', 'active=0.5 divisor=2'),
                    state.replace('checkerboard requested=1 active=0', 'checkerboard requested=1 active=1')):
            with self.assertRaises(RuntimeError):
                rc.verify_receiver_state(bad, 'full', 1, 'texture', True,
                                         full_texture=True, checkerboard_requested=True)
        model = rc.verify_model_receiver_state(state, True, True, 100000)
        for bad in (state.replace('models_active=1', 'models_active=0'), state.replace('draws=25', 'draws=0'),
                    state.replace('triangles=250000', 'triangles=4')):
            with self.assertRaises(RuntimeError):rc.verify_model_receiver_state(bad, True, True, 100000)
        before = {**model, 'frames_active': 0xfffffffe, 'frames_fallback': 7}
        after = {**model, 'frames_active': 3, 'frames_fallback': 7}
        self.assertEqual(rc.verify_model_receiver_frames(before, after, True)['delta'],
                         {'frames_active': 5, 'frames_fallback': 0})
        with self.assertRaises(RuntimeError):rc.verify_model_receiver_frames(before, {**after, 'frames_fallback':8}, True)
        with self.assertRaises(RuntimeError):rc.verify_model_receiver_frames(before, before, True)
        self.assertEqual(rc.verify_model_receiver_frames(before, {**before,'frames_fallback':8}, False)['delta']['frames_fallback'],1)

    def test_probe_filter_strength(self):
        for value in ('0', '0.5', '1', '1.75', '2'):
            self.assertEqual(rc.probe_gaussian_strength(value), float(value))
        for value in ('-0.1', '2.1', 'nan', 'inf', 'bad'):
            with self.assertRaises(argparse.ArgumentTypeError):
                rc.probe_gaussian_strength(value)
        rc.verify_probe_filter('"r_radiance_probe_gaussian" is "1.25" (default: "0")', 1.25)
        for output in ('unknown command', '"r_radiance_probe_gaussian" is "nan"',
                       '"r_radiance_probe_gaussian" is "1"'):
            with self.assertRaises(RuntimeError):
                rc.verify_probe_filter(output, 1.25)

    def test_competing_game_detection_ignores_benchmark_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for pid, name in (('12', 'vkquake'), ('34', 'vkQuake'), ('56', 'other')):
                (root / pid).mkdir()
                (root / pid / 'comm').write_text(name + '\n')
            (root / '78').mkdir()  # exited between listing and reading comm
            (root / 'nonnumeric').mkdir()
            self.assertEqual(list(rc.other_game_pids(root, {12})), [34])

    def test_gpu_timer_parsing_and_truncation(self):
        data = rc.parse_gpu('GPU frame time: 100 samples, 1.2345 ms mean\n'
                            'GPU frame median: 0.9876 ms (100/100 samples)\n')
        self.assertEqual(data['samples'], 100)
        self.assertAlmostEqual(data['mean_ms'], 1.2345)
        self.assertAlmostEqual(data['median_ms'], .9876)
        self.assertFalse(data['median_truncated'])
        self.assertTrue(rc.parse_gpu('GPU frame time: 100 samples, 1.0 ms mean\n'
                                     'GPU frame median: 0.5 ms (50/100 samples)')['median_truncated'])
        self.assertIsNone(rc.parse_gpu('GPU frame time: 2 samples, 0.5 ms mean')['median_ms'])
        with self.assertRaises(RuntimeError): rc.parse_gpu('GPU frame time: no completed samples')

    def test_p95_and_optional_receiver_stage_history(self):
        data = rc.parse_gpu('GPU frame time: 100 samples, 1.2 ms mean\n'
                            'GPU frame median: 1.0 ms (100/100 samples)\n'
                            'GPU frame p95: 2.0 ms (80/100 samples)')
        self.assertEqual(data['p95_ms'], 2)
        self.assertTrue(data['p95_truncated'])
        output = ('GPU world stage: 4 samples, 2.5 ms mean, 2.0 ms median, 3.0 ms p95 (4/4 samples)\n'
                  'GPU entities stage: 4 samples, 1.0 ms mean, 0.75 ms median, 2.0 ms p95 (4/4 samples)\n'
                  'GPU receiver_classify stage: 4 samples, 0.1 ms mean, 0.1 ms median, 0.2 ms p95 (4/4 samples)\n'
                  'GPU receiver_shade stage: 4 samples, 0.3 ms mean, 0.3 ms median, 0.4 ms p95 (4/4 samples)\n'
                  'GPU receiver_filter stage: 0 samples, 0.0 ms mean, 0.0 ms median, 0.0 ms p95 (0/0 samples)')
        stages = rc.parse_gpu_stages(output)
        self.assertEqual(stages['world']['p95_ms'], 3)
        self.assertEqual(stages['receiver_shade']['p95_ms'], .4)
        self.assertEqual(stages['receiver_filter']['samples'], 0)

    def test_resolution_and_filter_assertions_require_exact_active_path(self):
        for value, expected in [('1', 1), ('.5', .5), ('.333333333', 1/3), ('.25', .25)]:
            self.assertEqual(rc.resolution_scale(value), expected)
        for value in ['.2', '.4', '0', 'nan', 'inf', 'bad']:
            with self.assertRaises(argparse.ArgumentTypeError): rc.resolution_scale(value)
        text = ('Radiance: reduced receiver requested=1 active=1 checkerboard requested=1 active=1\n'
                'Radiance: receiver scale requested=0.333333 active=0.333333 divisor=3\n'
                'Radiance: receiver filter requested=texture active=texture')
        rc.verify_receiver_state(text, 'checkerboard', 1/3, 'texture', True)
        for changed in (text.replace('divisor=3', 'divisor=4'), text.replace('active=texture', 'active=probe'),
                        text.replace('active=0.333333', 'active=0.5'), text.replace('requested=texture', 'requested=probe')):
            with self.assertRaises(RuntimeError): rc.verify_receiver_state(changed, 'checkerboard', 1/3, 'texture', True)
        full = ('reduced receiver requested=0 active=0 checkerboard requested=0 active=0\n'
                'receiver scale requested=1 active=1 divisor=1\nreceiver filter requested=probe active=full')
        rc.verify_receiver_state(full, 'full', 1, 'probe', True)
        with self.assertRaises(RuntimeError): rc.verify_receiver_state('', 'full', 1, 'probe', True)
        rc.verify_hold('"host_timescale" is "0.000001"')
        for output in ('', '"host_timescale" is "nan"', '"host_timescale" is "1"', '"host_timescale" is "bad"'):
            with self.assertRaises(RuntimeError): rc.verify_hold(output)
        rc.verify_receiver_diagnostics_off('"r_radiance_receiver_diagnostics" is "0"')
        for output in ('', '"r_radiance_receiver_diagnostics" is "1"', '"r_radiance_receiver_diagnostics" is "nan"'):
            with self.assertRaises(RuntimeError): rc.verify_receiver_diagnostics_off(output)
        rc.verify_pose('Viewpos: (607 143 40) 2 90 0', (607, 143, 40, 3, 91, 0))
        with self.assertRaises(RuntimeError): rc.verify_pose('Viewpos: (0 0 0) 0 0 0', (607, 143, 40, 3, 91, 0))

    def test_two_thirds_scale_is_fractional_and_keeps_odd_edges(self):
        for text in ('.666', '.666666667', '.666667', '2/3'):
            self.assertEqual(rc.resolution_scale(text), 2/3)
        self.assertEqual(rc.receiver_divisor(2/3), 1.5)
        self.assertIsInstance(rc.receiver_divisor(.5), int)
        self.assertEqual((rc.receiver_extent(641, 2/3), rc.receiver_extent(361, 2/3)), (428, 241))
        self.assertEqual([rc.receiver_extent(n, 2/3) for n in range(13)],
                         [0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 8, 8])
        text = ('reduced receiver requested=1 active=1 checkerboard requested=1 active=1\n'
                'receiver scale requested=0.666667 active=0.666667 divisor=1.5\n'
                'receiver filter requested=texture active=texture')
        rc.verify_receiver_state(text, 'checkerboard', 2/3, 'texture', True)
        for bad in (text.replace('divisor=1.5', 'divisor=2'), text.replace('divisor=1.5', 'divisor=5'),
                    text.replace('active=0.666667', 'active=0.5'),
                    text.replace('active=0.666667 divisor=1.5', 'active=1 divisor=1')):
            with self.assertRaises(RuntimeError): rc.verify_receiver_state(bad, 'checkerboard', 2/3, 'texture', True)

    def test_four_way_order_and_source_consistency(self):
        modes = ('full_direct', 'full_indirect', 'probe_reduced', 'texture_reduced')
        orders = [rc.comparison_order(pair, modes) for pair in range(4)]
        self.assertTrue(all(set(order) == set(modes) for order in orders))
        self.assertTrue(all(len({order[position] for order in orders}) == 4 for position in range(4)))
        predecessors = [(order[index], order[index+1]) for order in orders for index in range(3)]
        self.assertEqual(len(set(predecessors)), 12)
        text = 'Radiance: gains point=7 emissive=1 bounce=8\nRadiance: lights=737 emissive=72 emissive_omitted=0'
        self.assertEqual(rc.verify_sources(text, 7, 1, 8), (737, 72, 0))
        with self.assertRaises(RuntimeError): rc.verify_sources(text, 1, 1, 8)
        cohort_modes = tuple(str(index) for index in range(14))
        cohort_orders = [rc.comparison_order(pair, cohort_modes) for pair in range(14)]
        self.assertEqual(len({(order[index], order[index+1]) for order in cohort_orders for index in range(13)}), 14 * 13)

    def test_rejects_fallback_and_wrong_backend(self):
        self.assertEqual(rc.verify_state('Radiance: requested=0 active=0 backend=none quality=1', 'classic', 'auto'), 'none')
        text = 'Radiance: requested=1 active=1 backend=ray-query quality=1'
        self.assertEqual(rc.verify_state(text, 'radiance', 'hardware'), 'ray-query')
        self.assertEqual(rc.verify_state(text, 'radiance', 'hardware', 1), 'ray-query')
        with self.assertRaises(RuntimeError): rc.verify_state(text, 'radiance', 'hardware', 0)
        with self.assertRaises(RuntimeError): rc.verify_state(text, 'radiance', 'software')
        with self.assertRaises(RuntimeError): rc.verify_state('Radiance: requested=1 active=0 backend=none', 'radiance', 'auto')

    def test_reduced_receiver_requires_real_active_path(self):
        rc.verify_receiver_state('', 'full')
        rc.verify_receiver_state('Radiance: reduced receiver requested=1 active=1 checkerboard requested=0 active=0', 'half')
        rc.verify_receiver_state('Radiance: reduced receiver requested=1 active=1 checkerboard requested=1 active=1', 'checkerboard')
        for value in ('', 'Radiance: reduced receiver requested=1 active=0 checkerboard requested=1 active=0',
                      'Radiance: reduced receiver requested=1 active=1 checkerboard requested=1 active=0'):
            with self.assertRaises(RuntimeError):
                rc.verify_receiver_state(value, 'checkerboard')

    def test_readiness_requires_completed_prepass_and_static_scene(self):
        active = 'Radiance: requested=1 active=1 backend=ray-query quality=1\n'
        scene = 'Radiance: last frame scene_upload=0 B BLAS_upload static=0 B dynamic=128 B; builds static=0 dynamic=1 TLAS=1;\n'
        done = 'Radiance: completed GPU prepass=0.123 ms (receiver shading excluded)\n'
        self.assertTrue(rc.radiance_ready(active + scene + done))
        self.assertFalse(rc.radiance_ready(active + scene + 'Radiance: GPU timing unavailable\n'))
        self.assertFalse(rc.radiance_ready(active + scene.replace('static=0 B', 'static=128 B') + done))
        self.assertFalse(rc.radiance_ready(active + scene.replace('builds static=0', 'builds static=1') + done))
        self.assertFalse(rc.radiance_ready(active.replace('active=1', 'active=0') + scene + done))

    def test_stage_timing_parser(self):
        output = ('GPU world stage: 4 samples, 2.5000 ms mean, 2.0000 ms median (4/4 samples)\n'
                  'GPU entities stage: 4 samples, 1.0000 ms mean, 0.7500 ms median (3/4 samples)\n')
        stages = rc.parse_gpu_stages(output)
        self.assertEqual(stages['world']['median_ms'], 2.0)
        self.assertFalse(stages['world']['median_truncated'])
        self.assertTrue(stages['entities']['median_truncated'])
        with self.assertRaises(RuntimeError): rc.parse_gpu_stages(output.splitlines()[0])

    def test_paired_deltas_and_alternating_order(self):
        def sample(mean, median, wall, truncated=False):
            return {'whole_frame_gpu': {'mean_ms': mean, 'median_ms': median, 'median_truncated': truncated},
                    'whole_frame_wall_ms': wall}
        rows = [{'classic': sample(1, .8, 2), 'radiance': sample(3, 2, 4)},
                {'classic': sample(2, 1.5, 3), 'radiance': sample(3, 2, 4)},
                {'classic': sample(1, .6, 2), 'radiance': sample(4, 2, 5)}]
        summary = rc.summarize_pairs(rows)
        self.assertEqual(summary['median_paired_gpu_mean_delta_ms'], 2)
        self.assertAlmostEqual(summary['median_paired_gpu_frame_median_delta_ms'], 1.2)
        self.assertEqual(summary['median_paired_whole_frame_wall_delta_ms'], 2)
        rows[1]['classic']['whole_frame_gpu']['median_truncated'] = True
        self.assertIsNone(rc.summarize_pairs(rows)['median_paired_gpu_frame_median_delta_ms'])
        self.assertEqual(rc.alternating_order(0), ('classic', 'radiance'))
        self.assertEqual(rc.alternating_order(1), ('radiance', 'classic'))
        confidence = rc.paired_bootstrap([1, 2, 3], [10, 20, 30], repetitions=1000)
        self.assertEqual(confidence['pair_count'], 3)
        self.assertEqual(confidence['delta_ms'], [1, 3])
        self.assertEqual(confidence['change_percent'], [10, 30])
        self.assertEqual(confidence, rc.paired_bootstrap([1, 2, 3], [10, 20, 30], repetitions=1000))

    def test_filter_comparison_uses_same_delta_and_alternates(self):
        def sample(gpu, wall):
            return {'whole_frame_gpu': {'mean_ms': gpu, 'median_ms': gpu, 'median_truncated': False},
                    'whole_frame_wall_ms': wall}
        rows = [{'filter_base': sample(2, 3), 'filter_compare': sample(3, 5)},
                {'filter_base': sample(4, 6), 'filter_compare': sample(6, 7)}]
        summary = rc.summarize_pairs(rows, 'filter_base', 'filter_compare')
        self.assertEqual(summary['median_paired_gpu_frame_median_delta_ms'], 1.5)
        self.assertEqual(summary['median_paired_whole_frame_wall_delta_ms'], 1.5)
        self.assertEqual(rc.alternating_order(0, 'filter_base', 'filter_compare'), ('filter_base', 'filter_compare'))
        self.assertEqual(rc.alternating_order(1, 'filter_base', 'filter_compare'), ('filter_compare', 'filter_base'))

    def test_filter_comparison_runs_alternating_radiance_modes(self):
        engines = []

        class Engine:
            def __init__(self, config, work):
                self.process = SimpleNamespace(pid=123)
                self.filter = None
                self.settings = []
                self.closed = False
                engines.append(self)

            def wait(self, predicate):
                return predicate('CL_SignonReply: 4')

            def cmd(self, command):
                if command.startswith('r_radiance_probe_gaussian '):
                    self.filter = float(command.split()[1])
                    self.settings.append(self.filter)
                if command == 'r_radiance_probe_gaussian':
                    return f'"r_radiance_probe_gaussian" is "{self.filter}"'
                if command == 'radiance_stats':
                    return ('Radiance: requested=1 active=1 backend=ray-query quality=1\n'
                            'Radiance: BLAS_upload static=0 B dynamic=0 B; builds static=0\n'
                            'Radiance: completed GPU prepass=0.1 ms (receiver shading excluded)')
                if command.startswith('gpu_frame_time stop'):
                    return 'GPU frame time: 2 samples, 1.0 ms mean\nGPU frame median: 1.0 ms (2/2 samples)'
                return ''

            def close(self):
                self.closed = True

        class Counter:
            def __init__(self, pid, binary):
                self.count = 0

            def sample(self):
                self.count += 1
                return self.count, self.count * 60

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'vkquake'
            binary.write_bytes(b'test')
            (root / 'pak0.pak').write_bytes(b'test')
            output = root / 'results'
            argv = ['bench_radiance.py', '--binary', str(binary), '--basedir', str(root),
                    '--scenes', 'start', '--backend', 'hardware', '--pairs', '2', '--seconds', '1',
                    '--warmup', '0', '--prepass-samples', '0', '--probe-gaussian', '0',
                    '--compare-probe-gaussian', '1', '--output', str(output)]
            fake_modules = {'bench_meshlets': SimpleNamespace(BOOT='map start', Engine=Engine),
                            'bench_ecs_ripple': SimpleNamespace(FrameCounter=Counter)}
            with mock.patch.dict(sys.modules, fake_modules), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep'), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc.main()
            report = json.loads((output / 'results.json').read_text())
            self.assertTrue(report['complete'])
            self.assertEqual(engines[0].settings, [0, 1, 1, 0])
            self.assertEqual(report['compare_probe_gaussian'], 1)
            self.assertEqual(set(report['scenes']['start']['pairs'][0]), {'filter_base', 'filter_compare'})
            self.assertEqual(report['scenes']['start']['summary']['median_paired_gpu_frame_median_delta_ms'], 0)
            self.assertTrue(engines[0].closed)

            # A game starting during a timed sample invalidates the report and
            # closes only the benchmark's own engine.
            argv[-1] = str(root / 'interrupted')
            with mock.patch.dict(sys.modules, fake_modules), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(rc, 'ensure_no_game', side_effect=[None, None, None, RuntimeError('other game')]), \
                    mock.patch.object(rc.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, 'other game'):
                    rc.main()
            interrupted = json.loads((root / 'interrupted' / 'results.json').read_text())
            self.assertFalse(interrupted['complete'])
            self.assertTrue(engines[-1].closed)

    def test_matrix_cohorts_share_full_references_and_keep_fixed_cameras(self):
        import bench_radiance_receiver_matrix as matrix
        items = matrix.workloads(list(matrix.PRIMARY_CAMERAS), [(1280, 720), (1720, 1370)], [.5, 1/3, .25], [0, 1], [1, 2])
        groups = matrix.cohorts(items)
        self.assertEqual(len(items), 48)
        self.assertEqual(len(groups), 8)
        self.assertTrue(all(len(group['settings']) == 6 for group in groups))
        self.assertEqual(groups[0]['pose'], [607, 143, 40, 3, 91, 0])
        self.assertEqual(groups[-1]['pose'], [1312, 680, 688, 55, 90, 0])
        expanded = matrix.workloads(list(matrix.PRIMARY_CAMERAS), [(1280, 720), (1720, 1370)],
                                    [2/3, .5, 1/3, .25], [0, 1], [1, 2])
        self.assertEqual(len(expanded), 64)
        self.assertTrue(all(len(group['settings']) == 8 for group in matrix.cohorts(expanded)))
        self.assertEqual(expanded[0]['divisor'], 1.5)
        self.assertIn('div1.5-', expanded[0]['name'])
        control = matrix.cohorts(matrix.workloads(['start'], [(1280, 720)], [.5, .25], [0, 1], [0]))
        self.assertEqual(len(control), 1)
        self.assertEqual(control[0]['pose'], [544, 288, 32, -15, 90, 0])
        with self.assertRaises(ValueError): matrix.workloads(['doorway'], [(1280, 720)], [.5, .5], [0], [1])
        self.assertEqual(matrix.viewport('1720x1370'), (1720, 1370))
        with self.assertRaises(argparse.ArgumentTypeError): matrix.viewport('100x100')

    def test_matrix_freezes_binary_and_resumes_completed_cohorts(self):
        import bench_radiance_receiver_matrix as matrix
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / 'vkquake'
            binary.write_bytes(b'frozen executable')
            output = root / 'matrix'
            argv = ['bench_radiance_receiver_matrix.py', '--binary', str(binary), '--basedir', str(root),
                    '--cameras', 'doorway', '--output', str(output)]

            def run(command):
                child = Path(command[command.index('--output') + 1])
                child.mkdir()
                (child / 'results.json').write_text(json.dumps({'complete': True, 'scenes': {'e1m1': {'summary': {}}}}))
                self.assertIn('--hold-simulation', command)
                self.assertIn('--receiver-scales', command)
                self.assertIn('--receiver-checkerboards', command)
                self.assertIn('--production-full-reference', command)
                self.assertNotIn('--gpu-stages', command)
                self.assertNotIn('--allow-existing', command)
                return SimpleNamespace(returncode=0)

            with mock.patch.object(sys, 'argv', argv), mock.patch.object(matrix.subprocess, 'run', side_effect=run) as execute, \
                    mock.patch.object(matrix.subprocess, 'check_output', return_value='RTX test, driver test'), \
                    contextlib.redirect_stdout(io.StringIO()):
                matrix.main()
            report = json.loads((output / 'matrix.json').read_text())
            self.assertTrue(report['complete'])
            self.assertEqual(execute.call_count, 4)
            self.assertEqual((output / 'binary/vkquake').read_bytes(), b'frozen executable')
            binary.write_bytes(b'new production build')
            # Resume uses the immutable saved executable even if a later build
            # has replaced the original path.
            with mock.patch.object(sys, 'argv', argv + ['--resume']), mock.patch.object(matrix.subprocess, 'run') as execute, \
                    contextlib.redirect_stdout(io.StringIO()):
                matrix.main()
            execute.assert_not_called()

    def test_durable_summary_preserves_pairs_p95_and_all_references(self):
        import summarize_radiance_receiver_matrix as summary
        def sample(value):
            return {'whole_frame_gpu': {'mean_ms': value, 'median_ms': value, 'p95_ms': value+.5},
                    'whole_frame_wall_ms': value+1}
        rows = [{name: sample(value+offset) for name, value in [('full_direct', 4.1), ('full_indirect', 4),
                 ('probe_reduced', 5), ('texture_reduced', 2)]} for offset in (0, .1, .2)]
        child = {'mode_settings': {'texture_reduced': {'scale': .5, 'checkerboard': False}},
                 'scenes': {'e1m1': {'pairs': rows, 'summary': rc.receiver_comparisons(rows)}}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'results.json'
            path.write_text(json.dumps(child))
            matrix = {'complete': True, 'binary': {'sha256': 'test'}, 'cohorts': [{'name': 'doorway', 'scene': 'e1m1',
                'resolution': [1280, 720], 'gaussian': 1}], 'results': {'doorway': {'complete': True, 'result_path': str(path)}}}
            result = summary.performance(matrix)
            for row in rows:
                row.pop('probe_reduced')
                for name, value in row.items():
                    full = name.startswith('full_')
                    value['receiver_frames'] = {'delta': {'reduced': 0 if full else 10, 'full': 10 if full else 0}}
            child['scenes']['e1m1']['summary'] = rc.receiver_comparisons(rows)
            path.write_text(json.dumps(child))
            without_legacy = summary.performance(matrix)
            child['mode_settings']['texture_reduced']['scale'] = 2/3
            path.write_text(json.dumps(child))
            fractional = summary.performance(matrix)
        self.assertEqual(result['completed_cohorts'], 1)
        setting = result['settings'][0]
        self.assertEqual(set(setting['comparisons']), {'full_direct', 'full_indirect', 'probe'})
        self.assertEqual(setting['timings']['texture']['run_p95_ms'], [2.5, 2.6, 2.7])
        self.assertEqual(len(setting['comparisons']['full_indirect']['pairs']), 3)
        self.assertAlmostEqual(setting['comparisons']['full_indirect']['pairs'][0]['p95_delta_ms'], -2)
        self.assertLess(result['worst']['full_indirect']['confidence_upper']['change_percent_95'][1], 0)
        self.assertEqual(set(without_legacy['settings'][0]['comparisons']), {'full_direct', 'full_indirect'})
        self.assertNotIn('probe', without_legacy['settings'][0]['timings'])
        self.assertTrue(without_legacy['receiver_submission_integrity']['verified'])
        self.assertEqual(without_legacy['receiver_submission_integrity']['unexpected_full_in_reduced'], 0)
        self.assertFalse(result['receiver_submission_integrity']['verified'])
        self.assertEqual(fractional['settings'][0]['divisor'], 1.5)

    def test_diagnostic_coverage_domains_and_odd_viewport(self):
        from diagnose_radiance_receiver_filter import coverage
        text = ('estimated receiver coverage frame=42 viewport=641x361 divisor=4 output=filtered\n'
                'receiver blocks total=14651 empty=1000 reusable=10000 mixed=3651 raw_GI_valid=8000 selected_GI_valid=9000\n'
                'receiver pixels covered=180000 GI_reuse=150000 GI_fallback=30000 direct_reuse=170000 direct_fallback=10000 invalid_geometry=1000')
        value = coverage(text, 'texture', 4, 641, 361)
        self.assertEqual(value['blocks']['total'], 14651)
        self.assertAlmostEqual(value['GI_reuse_fraction'], 150000/180000)
        # Texture reconstruction reads raw output when both Gaussian and
        # checkerboarding are disabled; the filter pass is intentionally off.
        raw = coverage(text.replace('output=filtered', 'output=raw'), 'texture', 4, 641, 361, 'raw')
        self.assertEqual(raw['output'], 'raw')
        for changed in (text.replace('total=14651', 'total=14650'), text.replace('GI_fallback=30000', 'GI_fallback=30001'),
                        text.replace('output=filtered', 'output=raw'), text.replace('selected_GI_valid=9000', 'selected_GI_valid=10001')):
            with self.assertRaises(RuntimeError): coverage(changed, 'texture', 4, 641, 361)

    def test_two_thirds_coverage_uses_fractional_divisor_and_exact_grid(self):
        from diagnose_radiance_receiver_filter import coverage
        from smoke_radiance_receiver_filter import counters
        text = ('estimated receiver coverage frame=42 viewport=641x361 divisor=1.5 output=filtered\n'
                'receiver blocks total=103148 empty=1000 reusable=100000 mixed=2148 raw_GI_valid=80000 selected_GI_valid=90000\n'
                'receiver pixels covered=231401 GI_reuse=200000 GI_fallback=31401 direct_reuse=220000 direct_fallback=11401 invalid_geometry=1000')
        diagnostic = coverage(text, 'texture', 1.5, 641, 361)
        self.assertEqual(diagnostic['blocks']['total'], 428*241)
        self.assertEqual(counters(text, 1, 2/3)['divisor'], 1.5)
        for bad in (text.replace('divisor=1.5', 'divisor=2'), text.replace('divisor=1.5', 'divisor=5'),
                    text.replace('total=103148', 'total=103147')):
            with self.assertRaises(RuntimeError): coverage(bad, 'texture', 1.5, 641, 361)
            with self.assertRaises(RuntimeError): counters(bad, 1, 2/3)

    def test_actual_submission_windows_and_uint32_wrap(self):
        before = rc.receiver_frames('Radiance: receiver frames reduced=4294967294 full=12')
        after = rc.receiver_frames('Radiance: receiver frames reduced=4 full=12')
        self.assertEqual(rc.verify_receiver_frames(before, after, True)['delta'], {'reduced': 6, 'full': 0})
        full = rc.verify_receiver_frames({'reduced': 9, 'full': 0xfffffffe}, {'reduced': 9, 'full': 2}, False)
        self.assertEqual(full['delta'], {'reduced': 0, 'full': 4})
        with self.assertRaises(RuntimeError): rc.verify_receiver_frames({'reduced': 0, 'full': 0}, {'reduced': 100, 'full': 1}, True)
        with self.assertRaises(RuntimeError): rc.verify_receiver_frames({'reduced': 0, 'full': 0}, {'reduced': 1, 'full': 100}, False)
        with self.assertRaises(RuntimeError): rc.verify_receiver_frames(before, before, True)
        with self.assertRaises(RuntimeError): rc.receiver_frames('')

    def test_receiver_comparison_holds_startup_and_checks_all_four_modes(self):
        engines = []

        class Engine:
            change_sources = False
            inject_full_frame = False

            def __init__(self, config, work):
                self.process = SimpleNamespace(pid=123)
                self.values = {}
                self.measurements = []
                self.closed = False
                self.after_stop = False
                self.frames = {'reduced': 0, 'full': 0}
                self.boot = (work / 'id1/boot.cfg').read_text()
                self.cmd(self.boot)
                engines.append(self)

            def wait(self, predicate):
                return predicate('CL_SignonReply: 4')

            def cmd(self, command):
                if command in ('r_radiance_probe_gaussian', 'host_timescale', 'r_radiance_receiver_diagnostics'):
                    return f'"{command}" is "{self.values[command]}"'
                for line in command.splitlines():
                    fields = line.split()
                    if len(fields) == 2 and fields[0].startswith(('r_', 'host_timescale')):
                        self.values[fields[0]] = float(fields[1])
                if command == 'radiance_stats':
                    scale = self.values['r_radiance_resolution_scale']
                    reduced = int(scale != 1)
                    checker = reduced * int(self.values['r_radiance_checkerboard'])
                    filtering = 'texture' if self.values['r_radiance_receiver_filter'] else 'probe'
                    sources = 101 if self.change_sources and self.after_stop else 100
                    return (f'Radiance: requested=1 active=1 backend=ray-query quality=1\n'
                            f'Radiance: reduced receiver requested={reduced} active={reduced} checkerboard requested={checker} active={checker}\n'
                            f'Radiance: receiver scale requested={scale} active={scale} divisor={rc.receiver_divisor(scale):g}\n'
                            f'Radiance: receiver filter requested={filtering} active={filtering if reduced else "full"}\n'
                            f'Radiance: gains point=1 emissive=1 bounce=1\nRadiance: lights={sources} emissive=72 emissive_omitted=0\n'
                            f'Radiance: receiver frames reduced={self.frames["reduced"]} full={self.frames["full"]}\n'
                            'Radiance: BLAS_upload static=0 B dynamic=0 B; builds static=0\n'
                            'Radiance: completed GPU prepass=0.1 ms (receiver shading excluded)')
                if command.startswith('gpu_frame_time start'):
                    self.measurements.append(dict(self.values))
                    reduced = self.values['r_radiance_resolution_scale'] != 1
                    self.frames['reduced' if reduced else 'full'] += 2
                    if reduced and self.inject_full_frame: self.frames['full'] += 1
                if command.startswith('gpu_frame_time stop'):
                    self.after_stop = True
                    return ('GPU frame time: 2 samples, 1.0 ms mean\nGPU frame median: 1.0 ms (2/2 samples)\n'
                            'GPU frame p95: 1.1 ms (2/2 samples)')
                return ''

            def close(self):
                self.closed = True

        class Counter:
            def __init__(self, pid, binary): self.count = 0
            def sample(self):
                self.count += 1
                return self.count, self.count * 60
            def close(self): pass

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'vkquake').write_bytes(b'test')
            (root / 'pak0.pak').write_bytes(b'test')
            output = root / 'results'
            argv = ['bench_radiance.py', '--binary', str(root / 'vkquake'), '--basedir', str(root),
                    '--scenes', 'e1m1', '--backend', 'hardware', '--pairs', '5', '--prepass-samples', '0',
                    '--probe-gaussian', '2', '--resolution-scale', '.25', '--receiver', 'checkerboard',
                    '--receiver-filter', 'texture', '--compare-receiver-filters', '--production-full-reference',
                    '--hold-simulation', '--output', str(output)]
            modules = {'bench_meshlets': SimpleNamespace(BOOT='r_indirect 1\nmap start', Engine=Engine),
                       'bench_ecs_ripple': SimpleNamespace(FrameCounter=Counter)}
            with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(rc, 'SimulationClock', return_value=SimpleNamespace(sample=lambda: 1.0)), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep') as sleeps, \
                    contextlib.redirect_stdout(io.StringIO()):
                rc.main()
            report = json.loads((output / 'results.json').read_text())
            self.assertTrue(report['complete'])
            self.assertTrue(report['acceptance_timing'])
            self.assertTrue(report['hold_simulation'])
            self.assertEqual(sleeps.call_args_list.count(mock.call(10)), 4)
            engine = engines[-1]
            self.assertLess(engine.boot.index('host_timescale 0.000001'), engine.boot.index('map e1m1'))
            self.assertNotIn('pause', engine.boot)
            self.assertEqual(len(engine.measurements), 20)
            configurations = {(x['r_radiance_resolution_scale'], x['r_radiance_receiver_filter'],
                               x['r_radiance_checkerboard'], x['r_indirect']) for x in engine.measurements}
            self.assertEqual(configurations, {(1, 0, 0, 0), (1, 0, 0, 1), (.25, 0, 1, 0), (.25, 1, 1, 0)})
            self.assertEqual(len(report['scenes']['e1m1']['summary']), 5)
            self.assertTrue(engine.closed)

            cohort_argv = argv.copy()
            scale_index = cohort_argv.index('--resolution-scale')
            cohort_argv[scale_index:scale_index + 2] = ['--receiver-scales', '.5', '.333333333', '.25',
                                                       '--receiver-checkerboards', '0', '1']
            cohort_argv[-1] = str(root / 'cohort')
            with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, 'argv', cohort_argv), \
                    mock.patch.object(rc, 'SimulationClock', return_value=SimpleNamespace(sample=lambda: 1.0)), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep') as sleeps, \
                    contextlib.redirect_stdout(io.StringIO()):
                rc.main()
            cohort = json.loads((root / 'cohort/results.json').read_text())
            self.assertTrue(cohort['complete'])
            self.assertEqual(len(cohort['mode_settings']), 14)
            self.assertEqual(len(engines[-1].measurements), 70)
            self.assertEqual(sleeps.call_args_list.count(mock.call(10)), 4)
            self.assertEqual(len(cohort['scenes']['e1m1']['summary']), 30)

            fractional_argv = cohort_argv.copy()
            fractional_argv.insert(fractional_argv.index('--receiver-scales')+1, '.666')
            fractional_argv[-1] = str(root / 'fractional-cohort')
            with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, 'argv', fractional_argv), \
                    mock.patch.object(rc, 'SimulationClock', return_value=SimpleNamespace(sample=lambda: 1.0)), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep'), \
                    contextlib.redirect_stdout(io.StringIO()):
                rc.main()
            fractional = json.loads((root / 'fractional-cohort/results.json').read_text())
            self.assertEqual(len(fractional['mode_settings']), 18)
            self.assertEqual(len(engines[-1].measurements), 90)
            self.assertEqual(fractional['mode_settings']['texture_d1.5_c0']['scale'], 2/3)
            self.assertIn('probe_d1.5_c1', fractional['mode_settings'])
            self.assertIn('texture_d2_c0', fractional['mode_settings'])

            without_legacy = fractional_argv.copy()
            without_legacy[-1] = str(root / 'without-legacy')
            without_legacy.append('--no-legacy-reference')
            with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, 'argv', without_legacy), \
                    mock.patch.object(rc, 'SimulationClock', return_value=SimpleNamespace(sample=lambda: 1.0)), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep') as sleeps, \
                    contextlib.redirect_stdout(io.StringIO()):
                rc.main()
            no_legacy = json.loads((root / 'without-legacy/results.json').read_text())
            self.assertFalse(no_legacy['legacy_reference'])
            self.assertEqual(len(no_legacy['mode_settings']), 10)
            self.assertFalse(any(name.startswith('probe_') for name in no_legacy['mode_settings']))
            self.assertEqual(len(engines[-1].measurements), 50)
            self.assertEqual(len(no_legacy['scenes']['e1m1']['summary']), 16)
            self.assertEqual(sleeps.call_args_list.count(mock.call(10)), 3)

            Engine.change_sources = True
            argv[-1] = str(root / 'changed')
            with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(rc, 'SimulationClock', return_value=SimpleNamespace(sample=lambda: 1.0)), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep'), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, 'source counts changed during the timed sample'):
                    rc.main()
            self.assertFalse(json.loads((root / 'changed/results.json').read_text())['complete'])
            self.assertTrue(engines[-1].closed)

            Engine.change_sources = False
            Engine.inject_full_frame = True
            argv[-1] = str(root / 'hidden-fallback')
            with mock.patch.dict(sys.modules, modules), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(rc, 'SimulationClock', return_value=SimpleNamespace(sample=lambda: 1.0)), \
                    mock.patch.object(rc, 'ensure_no_game'), mock.patch.object(rc.time, 'sleep'), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, 'full-resolution receiver submissions occurred'):
                    rc.main()
            self.assertFalse(json.loads((root / 'hidden-fallback/results.json').read_text())['complete'])
            self.assertTrue(engines[-1].closed)


if __name__ == '__main__': unittest.main()
