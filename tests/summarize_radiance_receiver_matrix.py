#!/usr/bin/env python3
"""Summarize completed receiver cohorts, paired confidence, and capture changes."""
import argparse
import json
from pathlib import Path
import statistics

import bench_radiance as bench


def paired_rows(rows, reference, treatment):
    result = []
    for index, row in enumerate(rows, 1):
        first, second = row[reference]['whole_frame_gpu'], row[treatment]['whole_frame_gpu']
        valid_median = first.get('median_ms') and second.get('median_ms') is not None and not first.get('median_truncated') and not second.get('median_truncated')
        valid_p95 = first.get('p95_ms') is not None and second.get('p95_ms') is not None and not first.get('p95_truncated') and not second.get('p95_truncated')
        result.append({'pair': index, 'reference_median_ms': first.get('median_ms'), 'texture_median_ms': second.get('median_ms'),
            'median_delta_ms': second['median_ms'] - first['median_ms'] if valid_median else None,
            'median_change_percent': 100 * (second['median_ms'] / first['median_ms'] - 1) if valid_median else None,
            'reference_p95_ms': first.get('p95_ms'), 'texture_p95_ms': second.get('p95_ms'),
            'p95_delta_ms': second['p95_ms'] - first['p95_ms'] if valid_p95 else None})
    return result


def timing(rows, mode):
    medians = [row[mode]['whole_frame_gpu']['median_ms'] for row in rows]
    p95 = [row[mode]['whole_frame_gpu'].get('p95_ms') for row in rows]
    return {'run_medians_ms': medians, 'run_p95_ms': p95,
            'median_run_median_ms': statistics.median(medians) if all(value is not None for value in medians) else None,
            'median_run_p95_ms': statistics.median(p95) if all(value is not None for value in p95) else None}


def performance(matrix):
    settings = []
    completed = 0
    submissions = {'samples': 0, 'missing_samples': 0, 'invalid_samples': 0, 'reduced_samples': 0, 'full_samples': 0,
                   'reduced_frames': 0, 'full_frames': 0, 'unexpected_full_in_reduced': 0, 'unexpected_reduced_in_full': 0}
    for cohort in matrix['cohorts']:
        saved = matrix['results'].get(cohort['name'], {})
        if not saved.get('complete'): continue
        child = json.loads(Path(saved['result_path']).read_text())
        data = child['scenes'][cohort['scene']]
        rows = data['pairs']
        completed += 1
        for row in rows:
            for name, value in row.items():
                submissions['samples'] += 1
                counts = value.get('receiver_frames')
                if not counts:
                    submissions['missing_samples'] += 1
                    continue
                delta = counts['delta']
                full = name.startswith('full_')
                if not delta['full' if full else 'reduced'] or delta['reduced' if full else 'full']:
                    submissions['invalid_samples'] += 1
                submissions['full_samples' if full else 'reduced_samples'] += 1
                submissions['reduced_frames'] += delta['reduced']
                submissions['full_frames'] += delta['full']
                submissions['unexpected_reduced_in_full' if full else 'unexpected_full_in_reduced'] += delta['reduced' if full else 'full']
        for texture in (name for name in child['mode_settings'] if name.startswith('texture_')):
            mode = child['mode_settings'][texture]
            suffix = texture[len('texture_'):]
            probe = 'probe_' + suffix
            entry = {'cohort': cohort['name'], 'resolution': cohort['resolution'], 'gaussian': cohort['gaussian'],
                'scale': mode['scale'], 'divisor': bench.receiver_divisor(mode['scale']), 'checkerboard': mode['checkerboard'],
                'timings': {}, 'comparisons': {}, 'captures': {}, 'result_path': saved['result_path']}
            for name in (texture, probe, 'full_direct', 'full_indirect'):
                if name not in rows[0]: continue
                label = 'texture' if name == texture else 'probe' if name == probe else name
                entry['timings'][label] = timing(rows, name)
                entry['captures'][label] = rows[-1][name].get('screenshot')
            for label, reference in (('full_direct', 'full_direct'), ('full_indirect', 'full_indirect'), ('probe', probe)):
                key = texture.replace('_reduced', '') + '_vs_' + label
                # Single-setting comparisons retain the older compact keys.
                if suffix == 'reduced': key = 'texture_vs_' + label
                if reference in rows[0]:
                    pairs = paired_rows(rows, reference, texture)
                    comparison = {**data['summary'][key], 'pairs': pairs}
                    if not comparison.get('paired_bootstrap_95') and all(pair['median_delta_ms'] is not None and pair['median_change_percent'] is not None for pair in pairs):
                        comparison['paired_bootstrap_95'] = bench.paired_bootstrap([pair['median_delta_ms'] for pair in pairs], [pair['median_change_percent'] for pair in pairs])
                    entry['comparisons'][label] = comparison
            settings.append(entry)
    worst = {}
    for reference in ('full_direct', 'full_indirect', 'probe'):
        available = [entry for entry in settings if reference in entry['comparisons']]
        if not available: continue
        valid = [entry for entry in available if entry['comparisons'][reference].get('median_paired_gpu_frame_median_change_percent') is not None]
        confidence = [entry for entry in available if entry['comparisons'][reference].get('paired_bootstrap_95')]
        def identify(entry, field):
            return {key: entry[key] for key in ('cohort', 'scale', 'divisor', 'checkerboard')} | field
        if valid:
            entry = max(valid, key=lambda item: item['comparisons'][reference]['median_paired_gpu_frame_median_change_percent'])
            worst[reference] = {'median_effect': identify(entry, {'change_percent': entry['comparisons'][reference]['median_paired_gpu_frame_median_change_percent']})}
        if confidence:
            entry = max(confidence, key=lambda item: item['comparisons'][reference]['paired_bootstrap_95']['change_percent'][1])
            worst.setdefault(reference, {})['confidence_upper'] = identify(entry, {'change_percent_95': entry['comparisons'][reference]['paired_bootstrap_95']['change_percent']})
    submissions['verified'] = bool(submissions['samples']) and not any(submissions[name] for name in ('missing_samples', 'invalid_samples', 'unexpected_full_in_reduced', 'unexpected_reduced_in_full'))
    submissions['note'] = 'CPU submission windows include console-command boundary frames; totals are not GPU timer sample counts.'
    return {'complete': matrix['complete'], 'completed_cohorts': completed, 'total_cohorts': len(matrix['cohorts']),
            'receiver_submission_integrity': submissions,
            'binary': matrix['binary'], 'settings': settings, 'worst': worst,
            'note': 'Paired-run median effects and bootstrap intervals are whole-frame timing comparisons. Negative changes mean faster texture receivers.'}


def captures(report):
    from PIL import Image
    import numpy as np
    result = []
    for entry in report['settings']:
        paths = entry['captures']
        if not paths.get('texture'): continue
        image = np.asarray(Image.open(paths['texture']).convert('RGB'), dtype=np.int16)
        height, width = image.shape[:2]
        y, x = np.indices((height, width))
        mask = (y < height * .90) & ~((x >= width * .39) & (x < width * .62) & (y >= height * .36) & (y < height * .48))
        item = {key: entry[key] for key in ('cohort', 'scale', 'divisor', 'checkerboard')}
        item['comparisons'] = {}
        for reference in ('probe', 'full_direct', 'full_indirect'):
            if not paths.get(reference): continue
            other = np.asarray(Image.open(paths[reference]).convert('RGB'), dtype=np.int16)
            if other.shape != image.shape: raise RuntimeError('comparison captures have different sizes')
            error = np.abs(image-other)[mask]
            item['comparisons'][reference] = {'mean_abs_rgb_255': float(error.mean()),
                'p95_abs_rgb_255': float(np.percentile(error, 95)), 'p99_abs_rgb_255': float(np.percentile(error, 99)),
                'max_abs_rgb_255': int(error.max()), 'pixel_fraction_over_8': float((error.max(axis=1) > 8).mean()),
                'pixel_fraction_over_16': float((error.max(axis=1) > 16).mean())}
        result.append(item)
    return {'complete': report['complete'], 'settings': result,
            'note': 'RGB differences describe intentional reduced-resolution/bounce blur, not pixel equivalence. HUD and centerprint region are excluded; timing uses complete frames.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrix', type=Path)
    parser.add_argument('--captures', action='store_true')
    args = parser.parse_args()
    report = performance(json.loads(args.matrix.read_text()))
    output = args.matrix.parent / 'paired-bootstrap-summary.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    if args.captures:
        (args.matrix.parent / 'capture-differences.json').write_text(json.dumps(captures(report), indent=2) + '\n')
    print(f'{report["completed_cohorts"]}/{report["total_cohorts"]} cohorts, {len(report["settings"])} settings; {output}')
    for reference, value in report['worst'].items(): print('worst versus', reference, json.dumps(value))


if __name__ == '__main__': main()
