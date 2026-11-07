# SPDX-License-Identifier: GPL-2.0-or-later
"""Decide each surface's bake size and bake it only if nothing can be reused."""

from array import array
from dataclasses import dataclass
from pathlib import Path

import bpy

from . import bake_inputs
from .bake_cache import make_key
from .baking import bake_sockets, needs_generated_uv, source_material
from .model import ExportError
from .sizing import GENERATED_UV_COVERAGE, SizeChoice, achieved_density, choose_size, settle_size

_DIFFUSE, _GLOW, _UVS = 'diffuse.png', 'glow.png', 'uvs.bin'


@dataclass(frozen=True)
class BakedSurface:
    diffuse: Path
    glow: Path | None
    uvs: tuple | None


@dataclass(frozen=True)
class BakeRecord:
    surface: str
    size: int
    status: str  # 'baked', 'cached', or 'shared' with an identical surface.
    source: str  # 'AUTO', 'FIXED', or 'MATERIAL'.
    world_area: float
    uv_coverage: float
    density: float
    key: str


@dataclass(frozen=True)
class _Finished:
    diffuse: Path
    glow: Path | None
    uvs: Path | None
    coverage: float


class SurfaceBaker:
    """One export's bakes. ``open_session`` runs only when a bake is really needed."""

    def __init__(self, context, open_session, cache, policy, scratch, save_png, *, margin, samples,
                 uv_mode, apply_modifiers, scale, force):
        self.context, self.open_session, self.cache, self.policy = context, open_session, cache, policy
        self.scratch, self.save_png = scratch, save_png
        self.margin, self.samples, self.uv_mode = margin, samples, uv_mode
        self.apply_modifiers, self.scale, self.force = apply_modifiers, scale, force
        self.records, self.warnings = [], set()
        self.session = None
        self.hasher = bake_inputs.MaterialHasher()
        self.code = bake_inputs.code_digest()
        self.finished = {}
        self.notes = set()  # Coverage tables, spared from eviction like the bakes.
        self.limited = 0

    def _inputs(self, obj, mesh, material_index, source, sockets, uv_name, generate_uv):
        """Return what determines the layout, what also determines the bake, and
        whether the bake may persist. Neither includes the size."""
        material = self.hasher.material(source, sockets)
        scene = self.context.scene
        modified = self.apply_modifiers and len(obj.modifiers) > 0
        layout = {
            'code': self.code, 'blender': bpy.app.version_string, 'apply_modifiers': self.apply_modifiers,
            'simplify': [scene.render.use_simplify, scene.render.simplify_subdivision],
            'mesh': bake_inputs.mesh_digest(mesh, None if modified else material_index),
        }
        if modified:
            # Generated UVs are projected on the base mesh, then carried through
            # the modifiers, so the evaluated result alone does not settle them.
            layout['base_mesh'] = bake_inputs.mesh_digest(obj.data)
            layout['modifiers'] = bake_inputs.modifier_digest(obj)
        if generate_uv:
            # Island packing can weigh faces by the object's scale and rotation.
            layout['object_basis'] = [list(row) for row in obj.matrix_world.to_3x3()]
        inputs = {**layout, 'samples': self.samples, 'uv_mode': self.uv_mode, 'destination_uv': uv_name,
                  'generate_uv': generate_uv, 'material': material.digest}
        if material.whole_mesh:
            inputs['whole_mesh'] = bake_inputs.mesh_digest(mesh)
            inputs['texture_space'] = [list(obj.data.texspace_location), list(obj.data.texspace_size)]
        if material.object_bound:
            inputs['object'] = bake_inputs.object_digest(obj)
        if material.animated:
            inputs['frame'] = [scene.frame_current, scene.frame_subframe]
        return layout, inputs, not material.volatile

    def _session(self):
        if self.session is None:
            self.session = self.open_session()
        return self.session

    def _settle_generated(self, obj, mesh, material_index, uv_name, area, layout):
        """Find a generated layout's size, remembering what each projection measured.

        The table depends on geometry alone: material edits and density changes
        reuse it, and a surface that keeps its size then needs no projection.
        """
        key = make_key({**layout, 'layout_margin': self.margin})
        entry = None if self.force else self.cache.lookup(key)
        table = dict(entry.meta['coverage']) if entry else {}
        settled = dict(entry.meta['settled']) if entry else {}
        self.notes.add(key)

        def coverage_at(size):
            if str(size) not in table:
                uvs = self._session().generated_layout(obj, mesh, material_index, uv_name, size=size,
                                                       margin=min(self.margin, size // 2),
                                                       apply_modifiers=self.apply_modifiers)
                table[str(size)] = bake_inputs.tuple_coverage(mesh, material_index, uvs)
            return table[str(size)]

        # Answer a repeated request from memory: a fresh search starts from what
        # the table has learned since, so it could probe new sizes or land elsewhere.
        request = repr((self.policy.density, self.policy.minimum, self.policy.maximum, area))
        if request in settled:
            size, limit = settled[request]
            return SizeChoice(size, limit=limit), table[str(size)]
        choice = settle_size(self.policy, area, coverage_at, max(table.values(), default=GENERATED_UV_COVERAGE))
        settled[request] = [choice.size, choice.limit]
        try:
            self.cache.put(key, {}, {'coverage': table, 'settled': settled})
        except OSError as exc:
            self.warnings.add(f"Could not store bakes in {self.cache.root}: {exc}")
        return choice, table[str(choice.size)]

    def _bake(self, obj, mesh, material_index, uv_name, size, margin, directory):
        result = self._session().bake(obj, mesh, material_index, uv_name, apply_modifiers=self.apply_modifiers,
                                      uv_mode=self.uv_mode, size=size, margin=margin)
        directory.mkdir()
        files = {_DIFFUSE: directory / _DIFFUSE}
        self.save_png(files[_DIFFUSE], result.diffuse, None)
        if result.glow is not None:
            files[_GLOW] = directory / _GLOW
            self.save_png(files[_GLOW], result.glow, 1.0)
        if result.uvs is not None:
            # Only this slot's corners: other faces can change without touching this bake.
            _, corners = bake_inputs.surface_corners(mesh, material_index)
            files[_UVS] = directory / _UVS
            files[_UVS].write_bytes(array('f', (value for corner in corners.tolist()
                                                for value in result.uvs[corner])).tobytes())
        self.session.release(result)
        return files, None if result.uvs is None else bake_inputs.tuple_coverage(mesh, material_index, result.uvs)

    @staticmethod
    def _uvs(path, mesh, material_index):
        if path is None:
            return None
        values = array('f')
        values.frombytes(path.read_bytes())
        _, corners = bake_inputs.surface_corners(mesh, material_index)
        if len(values) != 2 * len(corners):
            raise ExportError(f"Cached bake UVs cover {len(values) // 2} corners, not {len(corners)}; use Force Rebake")
        uvs = [(0.0, 0.0)] * len(mesh.loops)
        for index, corner in enumerate(corners.tolist()):
            uvs[corner] = (values[2 * index], values[2 * index + 1])
        return tuple(uvs)

    def bake(self, obj, mesh, material_index, material, uv_name):
        source = source_material(obj, mesh, material_index)
        sockets = bake_sockets(source, mesh)
        generate_uv = needs_generated_uv(mesh, material_index, uv_name, obj.name, self.uv_mode)
        area = bake_inputs.world_area(mesh, material_index, obj.matrix_world) * self.scale * self.scale
        override = material.quiver_md5.bake_size_override if material else 0
        layout, inputs, persistent = self._inputs(obj, mesh, material_index, source, sockets, uv_name, generate_uv)
        coverage = None if generate_uv else bake_inputs.layer_coverage(mesh, material_index, uv_name)
        if generate_uv and self.policy.mode == 'AUTO' and not override:
            choice, coverage = self._settle_generated(obj, mesh, material_index, uv_name, area, layout)
        else:
            choice = choose_size(self.policy, area, GENERATED_UV_COVERAGE if coverage is None else coverage, override)
        size = choice.size
        margin = min(self.margin, size // 2)
        key = make_key({**inputs, 'size': size, 'margin': margin})
        finished, status = self.finished.get(key), 'shared'
        if finished is None and persistent and not self.force:
            entry = self.cache.lookup(key)
            if entry is not None:
                finished = _Finished(entry.path(_DIFFUSE), entry.path(_GLOW), entry.path(_UVS), entry.meta['coverage'])
                status = 'cached'
        surface = f'{obj.name}:{source.name if source else material_index}'
        if finished is None:
            status = 'baked'
            files, measured = self._bake(obj, mesh, material_index, uv_name, size, margin,
                                         self.scratch / str(len(self.finished)))
            coverage = coverage if measured is None else measured
            if persistent:
                try:
                    entry = self.cache.put(key, files, {'size': size, 'surface': surface, 'coverage': coverage})
                    files = {name: entry.path(name) for name in files}
                except OSError as exc:
                    self.warnings.add(f"Could not store bakes in {self.cache.root}: {exc}")
            else:
                self.warnings.add(f"{obj.name}: image sequences, movies, and UDIM tiles are rebaked on every export")
            finished = _Finished(files[_DIFFUSE], files.get(_GLOW), files.get(_UVS), coverage)
        self.finished[key] = finished
        self.limited += choice.limit == 'MAX'
        self.records.append(BakeRecord(surface, size, status, choice.source, area, finished.coverage,
                                       achieved_density(size, area, finished.coverage), key))
        return BakedSurface(finished.diffuse, finished.glow, self._uvs(finished.uvs, mesh, material_index))

    def finish(self):
        """Trim the cache and return warnings for the export report."""
        for problem in self.cache.problems:
            self.warnings.add(f"Discarded a damaged cached bake and baked it again ({problem})")
        if self.limited:
            self.warnings.add(f"{self.limited} surface(s) reached Max Size and fall below the target texel density")
        try:
            self.cache.evict(keep={*self.finished, *self.notes})
        except OSError as exc:
            self.warnings.add(f"Could not trim the bake cache: {exc}")
        return self.warnings
