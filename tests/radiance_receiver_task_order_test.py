#!/usr/bin/env python3
"""Check every reset/capture/publication order permitted by the real task DAG.

No threads, GPU, display, game assets, or compiler are needed. Preparation can
clear last frame's publication while capture reads its actual C eligibility
expression; all legal event schedules must keep an active draw capturable.
"""
import ast
from itertools import permutations
from pathlib import Path
import re
from types import SimpleNamespace

root = Path(__file__).resolve().parents[1]
source = (root / 'Quake/gl_rmain.c').read_text()
engine = (root / 'Quake/radiance_engine.c').read_text()


def body(text, name):
    match = re.search(r'\b' + name + r'\s*\([^;]*?\)\s*\{', text)
    assert match, name
    position, depth = match.end(), 1
    while depth:
        depth += (text[position] == '{') - (text[position] == '}')
        position += 1
    return text[match.end():position - 1]


render = body(source, 'R_RenderView')
edges = set(re.findall(r'Task_AddDependency\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', render))
capture = body(source, 'RCExperimentalCaptureBegin')
expression = re.search(r'rc_capture_requested\s*=\s*([^;]+);', capture)[1]
expression = expression.replace('&&', ' and ').replace('||', ' or ')
expression = re.sub(r'!\s*(?!=)', ' not ', expression).strip()
syntax = ast.parse(expression, mode='eval')
allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.Compare, ast.Gt,
           ast.Constant, ast.Call, ast.Name, ast.Load, ast.UnaryOp, ast.Not, ast.Attribute)
assert all(isinstance(node, allowed) for node in ast.walk(syntax)), expression
eligibility = compile(syntax, '<actual capture eligibility>', 'eval')
prepare = body(engine, 'R_RadianceFramePrepare')
assert prepare.index('RCFrame_ClearPublication') < prepare.rindex('R_RadianceSetStats')


def reaches(start, end, graph):
    pending, seen = [start], set()
    while pending:
        node = pending.pop()
        if node == end:
            return True
        if node not in seen:
            seen.add(node)
            pending += [target for origin, target in graph if origin == node]
    return False


# The new dependency cannot close a cycle: capture only leads toward draws.
new_edge = ('prepare_radiance', 'capture_task')
assert not reaches('capture_task', 'prepare_radiance', edges - {new_edge})
assert re.search(r'if\s*\(\s*capture_task\s*!=\s*INVALID_TASK_HANDLE\s*\)\s*'
                 r'Task_AddDependency\s*\(\s*prepare_radiance\s*,\s*capture_task\s*\)', render)


def schedules(graph, successful=True):
    first_last = {
        'begin_rendering_task': ('begin', 'begin'),
        'prepare_radiance': ('reset', 'publish'),
        'capture_task': ('capture', 'capture'),
        'draw_world_task': ('draw', 'draw'),
    }
    events = ('begin', 'reset', 'publish', 'capture', 'draw')
    ordering = {('reset', 'publish')}
    # Other view/light prerequisites can already be complete in these schedules.
    for a, b in graph:
        if a in first_last and b in first_last:
            ordering.add((first_last[a][1], first_last[b][0]))
    outcomes = []
    for order in permutations(events):
        rank = {event: i for i, event in enumerate(order)}
        if any(rank[a] >= rank[b] for a, b in ordering):
            continue
        published, captured, draw_active = True, None, None
        for event in order:
            if event == 'reset':
                published = False
            elif event == 'publish':
                published = successful
            elif event == 'capture':
                captured = eval(eligibility, {'__builtins__': {}}, {
                    'R_RadianceResolutionDivisor': lambda: 4,
                    'R_RadianceActive': lambda: published,
                    'indirect': False,
                    'r_radiance_deferred': SimpleNamespace(value=1),
                })
            elif event == 'draw':
                draw_active = published
        outcomes.append((order, captured, draw_active))
    assert outcomes, 'task DAG has no legal schedule'
    return outcomes


# Demonstrate that the regression actually detects the previous missing edge.
old = schedules(edges - {new_edge})
assert any(active and not captured for _, captured, active in old)
assert reaches('capture_task','draw_entities_task',edges), 'model draw must wait for capture publication'
current = schedules(edges)
assert all(not active or captured for _, captured, active in current), current
failed = schedules(edges, successful=False)
assert all(not active and not captured for _, captured, active in failed), failed
print(f'PASS: {len(current)} legal publication/capture schedules preserve active draws; '
      'old graph deterministically reproduces fallback and failed preparation stays disabled')
