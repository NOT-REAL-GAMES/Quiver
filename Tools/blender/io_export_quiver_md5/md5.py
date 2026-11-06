# SPDX-License-Identifier: GPL-2.0-or-later
"""MD5 version 10 writer targeting vkQuake's rerelease model loader."""

import math

from .model import ExportError, validate_model


def _number(value):
    # Nine significant digits round-trip the engine's float representation.
    return format(value, ".9g") if value else "0"


def _vector(value):
    return "( " + " ".join(_number(v) for v in value) + " )"


def _quaternion(xyz):
    return (*xyz, -math.sqrt(max(0.0, 1.0 - sum(v * v for v in xyz))))


def _multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def _inverse(q):
    return (-q[0], -q[1], -q[2], q[3])


def _rotate(q, point):
    # q * (point, 0) * inverse(q), expanded to avoid temporary quaternions.
    x, y, z, w = q
    px, py, pz = point
    tx = 2.0 * (y * pz - z * py)
    ty = 2.0 * (z * px - x * pz)
    tz = 2.0 * (x * py - y * px)
    return (px + w * tx + y * tz - z * ty,
            py + w * ty + z * tx - x * tz,
            pz + w * tz + x * ty - y * tx)


def _local_bind(joints):
    local = []
    for joint in joints:
        if joint.parent == -1:
            local.append((joint.position, joint.orientation))
            continue
        parent = joints[joint.parent]
        inverse = _inverse(_quaternion(parent.orientation))
        offset = tuple(a - b for a, b in zip(joint.position, parent.position))
        position = _rotate(inverse, offset)
        quaternion = _multiply(inverse, _quaternion(joint.orientation))
        # MD5 reconstructs the missing component as negative W.
        if quaternion[3] > 0:
            quaternion = tuple(-v for v in quaternion)
        local.append((position, quaternion[:3]))
    return local


def _bounds(model):
    low = [math.inf] * 3
    high = [-math.inf] * 3
    quaternions = [_quaternion(j.orientation) for j in model.joints]
    for surface in model.surfaces:
        for vertex in surface.vertices:
            position = [0.0, 0.0, 0.0]
            for weight in vertex.weights:
                joint = model.joints[weight.joint]
                rotated = _rotate(quaternions[weight.joint], weight.position)
                for axis in range(3):
                    position[axis] += weight.bias * (joint.position[axis] + rotated[axis])
            for axis in range(3):
                low[axis] = min(low[axis], position[axis])
                high[axis] = max(high[axis], position[axis])
    return low, high


def mesh_text(model):
    """Return a validated .md5mesh; input winding and UVs are already MD5-ready."""
    validate_model(model)
    lines = ["MD5Version 10", 'commandline ""', "",
             f"numJoints {len(model.joints)}", f"numMeshes {len(model.surfaces)}", "",
             "joints {"]
    for joint in model.joints:
        lines.append(f'    "{joint.name}" {joint.parent} {_vector(joint.position)} {_vector(joint.orientation)}')
    lines.extend(["}", ""])
    for surface in model.surfaces:
        lines.extend([f"// Surface: {surface.name}", "mesh {", f'    shader "{surface.shader}"',
                      f"    numverts {len(surface.vertices)}"])
        weight_offset = 0
        for i, vertex in enumerate(surface.vertices):
            lines.append(f"    vert {i} {_vector(vertex.uv)} {weight_offset} {len(vertex.weights)}")
            weight_offset += len(vertex.weights)
        lines.extend(["", f"    numtris {len(surface.triangles)}"])
        for i, triangle in enumerate(surface.triangles):
            lines.append(f"    tri {i} " + " ".join(str(v) for v in triangle))
        lines.extend(["", f"    numweights {weight_offset}"])
        index = 0
        for vertex in surface.vertices:
            for weight in vertex.weights:
                lines.append(f"    weight {index} {weight.joint} {_number(weight.bias)} {_vector(weight.position)}")
                index += 1
        lines.extend(["}", ""])
    return "\n".join(lines)


def bind_animation_text(model, frame_rate=10):
    """Return a matching one-frame bind pose, required even for static assets.

    Mesh joints are absolute; animation baseframe transforms are parent-local.
    No components are animated. vkQuake addresses poses through QuakeC frame
    numbers and currently assigns its own 0.1-second frame interval.
    """
    validate_model(model)
    if isinstance(frame_rate, bool) or not isinstance(frame_rate, int) or frame_rate <= 0:
        raise ExportError("Frame rate must be a positive integer.")
    low, high = _bounds(model)
    lines = ["MD5Version 10", 'commandline ""', "", "numFrames 1",
             f"numJoints {len(model.joints)}", f"frameRate {frame_rate}",
             "numAnimatedComponents 0", "", "hierarchy {"]
    for joint in model.joints:
        lines.append(f'    "{joint.name}" {joint.parent} 0 0')
    lines.extend(["}", "", "bounds {", f"    {_vector(low)} {_vector(high)}", "}", "", "baseframe {"])
    for position, orientation in _local_bind(model.joints):
        lines.append(f"    {_vector(position)} {_vector(orientation)}")
    lines.extend(["}", "", "frame 0 {", "}", ""])
    return "\n".join(lines)
