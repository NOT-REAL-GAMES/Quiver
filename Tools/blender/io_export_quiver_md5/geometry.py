# SPDX-License-Identifier: GPL-2.0-or-later
"""Geometry cleanup for owned export/bake copies; source meshes stay intact."""

import bmesh


def triangle_has_area(mesh, triangle):
    a, b, c = (mesh.vertices[index].co for index in triangle.vertices)
    return (b - a).cross(c - a).length_squared > 1e-20


def clean_degenerate_faces(mesh):
    """Remove collapsed polygons, keeping loose vertices and all source data.

    Keeping vertices preserves Generated-coordinate bounds, vertex groups, and
    vertex correspondence. A polygon is removed only when all its tessellated
    triangles have zero area. Callers skip any collapsed tessellation triangles
    in otherwise valid polygons when exporting or validating UVs.
    """
    mesh.calc_loop_triangles()
    valid_polygons = {triangle.polygon_index for triangle in mesh.loop_triangles
                      if triangle_has_area(mesh, triangle)}
    collapsed = {polygon.index for polygon in mesh.polygons if polygon.index not in valid_polygons}
    if not collapsed:
        return 0
    editable = bmesh.new()
    try:
        editable.from_mesh(mesh)
        editable.faces.ensure_lookup_table()
        bmesh.ops.delete(editable, geom=[editable.faces[index] for index in sorted(collapsed)],
                         context='FACES_ONLY')
        editable.to_mesh(mesh)
    finally:
        editable.free()
    mesh.update()
    return len(collapsed)
