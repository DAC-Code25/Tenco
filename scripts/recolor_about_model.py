#!/usr/bin/env python3
"""Apply the About-page preview color scheme to the normalized vehicle GLB.

The source SolidWorks model does not carry useful runtime colors after
conversion, so the About page uses this deterministic material pass for a
maintainable visual preview. Geometry and buffer data are left unchanged.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path
from typing import Iterable


GLB_MAGIC = 0x46546C67
GLB_VERSION = 2
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def _identity() -> list[list[float]]:
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def _matmul(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _translation(v: list[float]) -> list[list[float]]:
    m = _identity()
    m[0][3], m[1][3], m[2][3] = v
    return m


def _scale(v: list[float]) -> list[list[float]]:
    m = _identity()
    m[0][0], m[1][1], m[2][2] = v
    return m


def _quaternion(q: list[float]) -> list[list[float]]:
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return [
        [1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), 0],
        [2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx), 0],
        [2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy), 0],
        [0, 0, 0, 1],
    ]


def _node_matrix(node: dict) -> list[list[float]]:
    if "matrix" in node:
        matrix = node["matrix"]
        return [[matrix[col * 4 + row] for col in range(4)] for row in range(4)]

    matrix = _identity()
    if "translation" in node:
        matrix = _matmul(matrix, _translation(node["translation"]))
    if "rotation" in node:
        matrix = _matmul(matrix, _quaternion(node["rotation"]))
    if "scale" in node:
        matrix = _matmul(matrix, _scale(node["scale"]))
    return matrix


def _transform_point(matrix: list[list[float]], point: Iterable[float]) -> list[float]:
    x, y, z = point
    return [
        matrix[0][0] * x + matrix[0][1] * y + matrix[0][2] * z + matrix[0][3],
        matrix[1][0] * x + matrix[1][1] * y + matrix[1][2] * z + matrix[1][3],
        matrix[2][0] * x + matrix[2][1] * y + matrix[2][2] * z + matrix[2][3],
    ]


def _parse_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != GLB_VERSION or length != len(data):
        raise ValueError(f"{path} is not a valid GLB v2 file")

    offset = 12
    json_chunk: bytes | None = None
    bin_chunk: bytes | None = None
    while offset < len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk_data = data[offset : offset + chunk_length]
        offset += chunk_length

        if chunk_type == JSON_CHUNK:
            json_chunk = chunk_data
        elif chunk_type == BIN_CHUNK:
            bin_chunk = chunk_data

    if json_chunk is None or bin_chunk is None:
        raise ValueError(f"{path} must contain JSON and BIN chunks")

    return json.loads(json_chunk.decode("utf-8").rstrip("\x00 ")), bin_chunk


def _write_glb(path: Path, gltf: dict, bin_chunk: bytes) -> None:
    json_bytes = json.dumps(gltf, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    json_padding = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_padding

    bin_padding = (4 - len(bin_chunk) % 4) % 4
    bin_bytes = bin_chunk + (b"\x00" * bin_padding)

    total_length = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    output = bytearray()
    output += struct.pack("<III", GLB_MAGIC, GLB_VERSION, total_length)
    output += struct.pack("<II", len(json_bytes), JSON_CHUNK)
    output += json_bytes
    output += struct.pack("<II", len(bin_bytes), BIN_CHUNK)
    output += bin_bytes

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(output)


def _material(name: str, rgba: list[float], metallic: float, roughness: float) -> dict:
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": rgba,
            "metallicFactor": metallic,
            "roughnessFactor": roughness,
        },
    }


def _append_materials(gltf: dict) -> dict[str, int]:
    palette = {
        "body_dark_gray": _material("Tenco body dark gray", [0.14, 0.15, 0.16, 1], 0.05, 0.58),
        "bracket_dark_gray": _material("Tenco bracket dark gray", [0.18, 0.19, 0.2, 1], 0.12, 0.48),
        "chassis_silver": _material("Tenco chassis silver", [0.72, 0.74, 0.72, 1], 0.58, 0.28),
        "wheel_red": _material("Tenco drive wheel red", [0.78, 0.03, 0.025, 1], 0.02, 0.42),
        "wheel_center_silver": _material("Tenco wheel center silver", [0.86, 0.86, 0.82, 1], 0.72, 0.22),
        "rubber_black": _material("Tenco caster rubber black", [0.01, 0.01, 0.01, 1], 0.0, 0.7),
        "sensor_black": _material("Tenco sensor black", [0.018, 0.02, 0.022, 1], 0.06, 0.5),
        "mushroom_gray_white": _material("Tenco mushroom gray white", [0.86, 0.86, 0.82, 1], 0.08, 0.36),
    }

    gltf.setdefault("materials", [])
    indices: dict[str, int] = {}
    for key, material in palette.items():
        indices[key] = len(gltf["materials"])
        gltf["materials"].append(material)
    return indices


def _world_matrices(gltf: dict) -> list[list[list[float]]]:
    parents = {}
    for index, node in enumerate(gltf["nodes"]):
        for child in node.get("children", []):
            parents[child] = index

    roots = [index for index in range(len(gltf["nodes"])) if index not in parents]
    matrices = [None] * len(gltf["nodes"])

    def visit(index: int, parent: list[list[float]]) -> None:
        matrices[index] = _matmul(parent, _node_matrix(gltf["nodes"][index]))
        for child in gltf["nodes"][index].get("children", []):
            visit(child, matrices[index])

    for root in roots:
        visit(root, _identity())

    return matrices


def _primitive_bbox(gltf: dict, world: list[list[float]], primitive: dict) -> tuple[list[float], list[float]]:
    accessor_index = primitive["attributes"].get("POSITION")
    if accessor_index is None:
        return [0, 0, 0], [0, 0, 0]

    accessor = gltf["accessors"][accessor_index]
    local_min = accessor["min"]
    local_max = accessor["max"]
    points = [
        _transform_point(world, (x, y, z))
        for x in (local_min[0], local_max[0])
        for y in (local_min[1], local_max[1])
        for z in (local_min[2], local_max[2])
    ]
    return [min(point[i] for point in points) for i in range(3)], [max(point[i] for point in points) for i in range(3)]


def _center(minimum: list[float], maximum: list[float]) -> list[float]:
    return [(minimum[index] + maximum[index]) / 2 for index in range(3)]


def _size(minimum: list[float], maximum: list[float]) -> list[float]:
    return [maximum[index] - minimum[index] for index in range(3)]


def _is_drive_wheel(center: list[float], size: list[float], node_index: int) -> bool:
    if node_index in {13, 20, 25, 127}:
        return True
    return False


def _is_drive_wheel_center(node_index: int, center: list[float], size: list[float]) -> bool:
    if node_index in {14, 15, 128, 129, 183}:
        return True
    return (
        -0.84 <= center[1] <= -0.72
        and 0.08 <= abs(center[2]) <= 0.16
        and size[0] <= 0.17
        and size[1] <= 0.17
    )


def _is_caster_assembly(center: list[float], size: list[float], node_index: int) -> bool:
    if node_index in {131, 132, 136, 137, 138, 139, 140, 141, 142, 144, 145, 146, 147, 149, 150, 151, 152, 153, 154, 155, 156, 158, 160, 161, 162, 163, 164, 165}:
        return True
    return (
        -0.90 <= center[1] <= -0.78
        and abs(center[0]) >= 0.24
        and 0.03 <= abs(center[2]) <= 0.16
        and size[0] <= 0.13
        and size[1] <= 0.10
    )


def _is_chassis(center: list[float], size: list[float], node_index: int) -> bool:
    if node_index in {24, 34, 195}:
        return True
    return center[1] < -0.58 and size[0] >= 0.32 and size[2] >= 0.18 and size[1] <= 0.08


def _is_main_body(node_index: int, center: list[float], size: list[float]) -> bool:
    if node_index in {21, 28, 69, 168, 191, 192}:
        return True
    return -0.72 <= center[1] <= -0.36 and size[0] >= 0.10 and size[2] >= 0.10


def _is_lidar_or_camera(node_index: int, name: str, center: list[float], maximum: list[float]) -> bool:
    upper_camera_nodes = {73, 74, 75, 76}
    if node_index in upper_camera_nodes or node_index in {196, 197}:
        return True
    lowered = name.lower()
    return "lds" in lowered


def _is_lidar_bracket(node_index: int, center: list[float], size: list[float]) -> bool:
    if node_index in {122, 123, 124, 125, 187, 188, 189, 190, 194}:
        return True
    return center[0] < -0.28 and size[1] > 0.4


def _is_mushroom_head(node_index: int, name: str) -> bool:
    return node_index in {185, 193} or "RTK" in name or "GPS" in name


def recolor(input_path: Path, output_path: Path) -> None:
    gltf, bin_chunk = _parse_glb(input_path)
    materials = _append_materials(gltf)
    world_matrices = _world_matrices(gltf)

    for node_index, node in enumerate(gltf["nodes"]):
        mesh_index = node.get("mesh")
        if mesh_index is None:
            continue

        node_name = node.get("name", "")
        for primitive in gltf["meshes"][mesh_index].get("primitives", []):
            minimum, maximum = _primitive_bbox(gltf, world_matrices[node_index], primitive)
            center = _center(minimum, maximum)
            size = _size(minimum, maximum)

            material = materials["body_dark_gray"]
            if _is_chassis(center, size, node_index):
                material = materials["chassis_silver"]
            if _is_main_body(node_index, center, size):
                material = materials["body_dark_gray"]
            if _is_lidar_bracket(node_index, center, size):
                material = materials["bracket_dark_gray"]
            if _is_drive_wheel(center, size, node_index):
                material = materials["wheel_red"]
            if _is_drive_wheel_center(node_index, center, size):
                material = materials["wheel_center_silver"]
            if _is_caster_assembly(center, size, node_index):
                material = materials["rubber_black"]
            if _is_mushroom_head(node_index, node_name):
                material = materials["mushroom_gray_white"]
            if _is_lidar_or_camera(node_index, node_name, center, maximum):
                material = materials["sensor_black"]

            primitive["material"] = material

    gltf.pop("extensionsRequired", None)
    _write_glb(output_path, gltf, bin_chunk)


def main() -> None:
    parser = argparse.ArgumentParser(description="Recolor the normalized About-page vehicle GLB.")
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("src/carmodel_about_centered_uncolored.glb"),
        help="Input normalized, uncolored GLB.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("src/carmodel_about_centered.glb"),
        help="Output recolored GLB used by the About page.",
    )
    args = parser.parse_args()

    recolor(args.input, args.output)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
