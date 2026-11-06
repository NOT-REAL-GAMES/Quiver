# SPDX-License-Identifier: GPL-2.0-or-later
"""Blender-independent asset data shared by export backends.

Positions are in game/model space, joint orientations are MD5 quaternion XYZ
(the omitted W is negative), and each weight position is local to its joint.
The Blender adapter owns axes, winding, UV conversion, and weight reduction.
"""

from dataclasses import dataclass
import math
import re


class ExportError(ValueError):
    """An asset cannot be represented safely by the requested backend."""


@dataclass(frozen=True)
class Joint:
    name: str
    parent: int
    position: tuple[float, float, float]
    orientation: tuple[float, float, float]


@dataclass(frozen=True)
class Weight:
    joint: int
    bias: float
    position: tuple[float, float, float]


@dataclass(frozen=True)
class Vertex:
    uv: tuple[float, float]
    weights: tuple[Weight, ...]


@dataclass(frozen=True)
class Surface:
    name: str
    shader: str
    vertices: tuple[Vertex, ...]
    triangles: tuple[tuple[int, int, int], ...]


@dataclass(frozen=True)
class Model:
    joints: tuple[Joint, ...]
    surfaces: tuple[Surface, ...]


def split_surface(surface, max_vertices=65535):
    """Keep triangles/materials intact while fitting the backend index width."""
    if isinstance(max_vertices, bool) or not isinstance(max_vertices, int) or max_vertices < 3:
        raise ExportError("A surface chunk must allow at least three vertices.")
    if len(surface.vertices) <= max_vertices:
        return (surface,)
    chunks, vertices, triangles, indices = [], [], [], {}

    def flush():
        if triangles:
            chunks.append(Surface(f"{surface.name} part {len(chunks) + 1}", surface.shader,
                                  tuple(vertices), tuple(triangles)))
            vertices.clear()
            triangles.clear()
            indices.clear()

    for triangle in surface.triangles:
        if len(vertices) + sum(index not in indices for index in set(triangle)) > max_vertices:
            flush()
        remapped = []
        for index in triangle:
            if index not in indices:
                indices[index] = len(vertices)
                vertices.append(surface.vertices[index])
            remapped.append(indices[index])
        triangles.append(tuple(remapped))
    flush()
    return tuple(chunks)


def _vector(value, size, description):
    if len(value) != size:
        raise ExportError(f"{description} must contain {size} coordinates.")
    # vkQuake parses these as single-precision floats.
    if any(not math.isfinite(v) or abs(v) > 3.4028234e38 for v in value):
        raise ExportError(f"{description} contains an invalid or non-finite coordinate.")


def _name(value, description, max_bytes=None):
    if not value or any(ord(c) < 32 or ord(c) == 127 or c in '\\"' for c in value):
        raise ExportError(f"{description} must be nonempty and contain no quotes, backslashes, or control characters.")
    if max_bytes is not None and len(value.encode("utf-8")) > max_bytes:
        raise ExportError(f"{description} exceeds {max_bytes} UTF-8 bytes.")


def _index(value, limit, description):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < limit:
        raise ExportError(f"{description} is out of range.")


def validate_model(model):
    """Validate the conservative common vkQuake MD5 export limits.

    Keep backend constraints here rather than silently discarding information in
    serialization. The adapter may reduce influences before constructing data.
    """
    if not 1 <= len(model.joints) <= 256:
        raise ExportError("MD5 export requires between 1 and 256 joints.")
    if not model.surfaces:
        raise ExportError("The model contains no surfaces.")
    names = set()
    for i, joint in enumerate(model.joints):
        _name(joint.name, f"Joint {i} name", 31)
        if joint.name in names:
            raise ExportError(f"Duplicate joint name: {joint.name}.")
        names.add(joint.name)
        if isinstance(joint.parent, bool) or not isinstance(joint.parent, int) or not -1 <= joint.parent < i:
            raise ExportError(f"Joint {joint.name} must follow its parent in the hierarchy.")
        _vector(joint.position, 3, f"Joint {joint.name} position")
        _vector(joint.orientation, 3, f"Joint {joint.name} orientation")
        if sum(v * v for v in joint.orientation) > 1.000001:
            raise ExportError(f"Joint {joint.name} has an invalid quaternion orientation.")

    for surface in model.surfaces:
        _name(surface.name, "Surface name")
        # Reserve _00_00_glow.png inside MAX_QPATH (64 including NUL).
        if len(surface.shader) > 48 or not re.fullmatch(r"[A-Za-z0-9_/-]+", surface.shader):
            raise ExportError(f"Surface {surface.name} shader must be a relative ASCII path stem of at most 48 characters.")
        if surface.shader.startswith("/") or any(p in ("", ".", "..") for p in surface.shader.split("/")):
            raise ExportError(f"Surface {surface.name} shader must be a safe game-relative path stem.")
        if not 1 <= len(surface.vertices) <= 65535:
            raise ExportError(f"Surface {surface.name} must contain 1 to 65535 vertices; split larger meshes.")
        if not surface.triangles:
            raise ExportError(f"Surface {surface.name} contains no triangles.")
        for i, vertex in enumerate(surface.vertices):
            _vector(vertex.uv, 2, f"Surface {surface.name}, vertex {i} UV")
            if not 1 <= len(vertex.weights) <= 4:
                raise ExportError(f"Surface {surface.name}, vertex {i} must have 1 to 4 weights.")
            total = 0.0
            influences = set()
            for weight in vertex.weights:
                _index(weight.joint, len(model.joints), f"Surface {surface.name}, vertex {i} joint")
                if weight.joint in influences:
                    raise ExportError(f"Surface {surface.name}, vertex {i} has duplicate joint weights.")
                influences.add(weight.joint)
                if not math.isfinite(weight.bias) or not 0.0 < weight.bias <= 1.0:
                    raise ExportError(f"Surface {surface.name}, vertex {i} has an invalid weight bias.")
                _vector(weight.position, 3, f"Surface {surface.name}, vertex {i} weight position")
                total += weight.bias
            if abs(total - 1.0) > 1e-5:
                raise ExportError(f"Surface {surface.name}, vertex {i} weights must sum to one.")
        for i, triangle in enumerate(surface.triangles):
            if len(triangle) != 3:
                raise ExportError(f"Surface {surface.name}, triangle {i} must contain three indices.")
            for index in triangle:
                _index(index, len(surface.vertices), f"Surface {surface.name}, triangle {i} vertex")
            if len(set(triangle)) != 3:
                raise ExportError(f"Surface {surface.name}, triangle {i} repeats a vertex.")
