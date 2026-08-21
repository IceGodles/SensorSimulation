#!/usr/bin/env python3
"""校验 SensorSimulation 当前 ChannelGuid + LiDAR v1/v2 数据集协议。"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

PAYLOAD_PREFIX = {1: "rgb_{guid}.png", 2: "semantic_{guid}.png", 4: "depth_meters_f32_{guid}.bin", 8: "instance_u32_{guid}.bin"}
LIDAR_V2_HEADER = struct.Struct("<IHHIIIIII")
LIDAR_V2_POINT = struct.Struct("<ffffHHIf")
LIDAR_MAGIC = 0x52444C53


def load_json(path: Path) -> Dict:
    return json.loads(path.read_text(encoding="utf-8"))


def find_frame_dirs(session_dir: Path) -> List[Path]:
    return sorted(item for item in session_dir.iterdir() if item.is_dir() and item.name.startswith("frame_") and not item.name.endswith(".tmp"))


def guid_digits(value: str) -> str:
    return value.replace("-", "").upper()


def expected_frame_files(frame_info: Dict) -> Set[str]:
    expected = {"frame_info.json", "groundtruth.json"}
    for image in frame_info.get("images", []):
        template = PAYLOAD_PREFIX.get(int(image["payload_type"]))
        if template:
            expected.add(template.format(guid=guid_digits(image["channel_guid"])))
    scans = frame_info.get("lidar_scans", [])
    for scan in scans:
        suffix = "" if len(scans) == 1 else "_" + guid_digits(scan["sensor_guid"])
        expected.update({f"lidar{suffix}.bin", f"lidar_extended{suffix}.bin"})
    return expected


def validate_basic_lidar(path: Path) -> Tuple[bool, str, int]:
    data = path.read_bytes()
    if len(data) % 16:
        return False, f"size {len(data)} is not divisible by 16", 0
    count = 0
    for count, point in enumerate(struct.iter_unpack("<ffff", data), start=1):
        x, y, z, intensity = point
        if not all(math.isfinite(value) for value in point):
            return False, f"point {count - 1} contains NaN/Inf", count
        if max(abs(x), abs(y), abs(z)) > 1000.0:
            return False, f"point {count - 1} exceeds 1000 m", count
        if not 0.0 <= intensity <= 1.0:
            return False, f"point {count - 1} intensity outside [0,1]", count
    return True, f"{count} points", count


def validate_extended_lidar(path: Path, expected_points: int) -> Tuple[bool, str]:
    data = path.read_bytes()
    if len(data) < LIDAR_V2_HEADER.size:
        return False, "missing 32-byte v2 header"
    header = LIDAR_V2_HEADER.unpack_from(data)
    magic, version, header_size, stride, point_count, flags, coordinate, endian, reserved = header
    if (magic, version, header_size, stride, flags, coordinate, endian, reserved) != (LIDAR_MAGIC, 2, 32, 28, 0x7, 1, 0x01020304, 0):
        return False, "invalid v2 header contract"
    if point_count != expected_points:
        return False, f"v2 point_count {point_count} != v1 {expected_points}"
    if len(data) != header_size + point_count * stride:
        return False, "v2 file size does not match header"
    previous_time = -1.0
    for index in range(point_count):
        x, y, z, intensity, semantic_id, point_reserved, instance_id, relative_time = LIDAR_V2_POINT.unpack_from(data, header_size + index * stride)
        if not all(math.isfinite(value) for value in (x, y, z, intensity, relative_time)):
            return False, f"v2 point {index} contains NaN/Inf"
        if not 0.0 <= intensity <= 1.0 or relative_time < previous_time or point_reserved != 0:
            return False, f"v2 point {index} has invalid intensity/time/reserved field"
        previous_time = relative_time
    return True, f"v2 schema OK ({point_count} points)"


def validate_semantic_image(path: Path, valid_ids: Set[int]) -> Tuple[bool, str]:
    try:
        from PIL import Image
    except ImportError:
        return True, "Pillow unavailable; pixel validation skipped"
    image = Image.open(path).convert("RGBA")
    pixels = image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()
    for index, (r, g, b, a) in enumerate(pixels):
        if r not in valid_ids or g != 0 or b != 0 or a != 255:
            return False, f"invalid RGBA at linear pixel {index}: {(r, g, b, a)}"
    return True, f"all {image.width * image.height} pixels valid"


def derive_semantic_ids(frame_dirs: List[Path]) -> Set[int]:
    ids = {0}
    for frame_dir in frame_dirs:
        path = frame_dir / "groundtruth.json"
        if path.exists():
            ids.update(int(item["semantic_id"]) for item in load_json(path).get("objects", []))
    return ids


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session_dir")
    parser.add_argument("--semantic-ids", help="逗号分隔；省略时从 GroundTruth 自动发现")
    parser.add_argument("--skip-pixels", action="store_true")
    parser.add_argument("--report", help="可选 JSON 报告路径")
    args = parser.parse_args()
    session_dir = Path(args.session_dir)
    metadata_path = session_dir / "metadata.json"
    calibration_path = session_dir / "calibration.json"
    errors: List[str] = []
    metadata = load_json(metadata_path) if metadata_path.exists() else {}
    if not metadata_path.exists(): errors.append("metadata.json missing")
    if not calibration_path.exists(): errors.append("calibration.json missing")
    if not (session_dir / "COMPLETED").exists(): errors.append("COMPLETED session marker missing")
    if metadata and not metadata.get("consistency", {}).get("passed", False):
        errors.append("metadata consistency checks did not pass")
    frame_dirs = find_frame_dirs(session_dir)
    committed = int(metadata.get("statistics", {}).get("export_committed_frames", len(frame_dirs)))
    if committed != len(frame_dirs): errors.append(f"committed={committed} but frame directories={len(frame_dirs)}")
    temp_dirs = list(session_dir.glob("frame_*.tmp"))
    if temp_dirs: errors.append(f"{len(temp_dirs)} temporary frame directories remain")
    temp_files = list(session_dir.glob("*.tmp"))
    if temp_files: errors.append(f"{len(temp_files)} temporary session files remain")
    valid_ids = {int(value) for value in args.semantic_ids.split(",")} if args.semantic_ids else derive_semantic_ids(frame_dirs)
    total_points = semantic_images = lidar_files = 0
    frame_ids: List[int] = []
    for frame_dir in frame_dirs:
        try: frame_ids.append(int(frame_dir.name.removeprefix("frame_")))
        except ValueError:
            errors.append(f"invalid frame directory name: {frame_dir.name}"); continue
        info_path = frame_dir / "frame_info.json"
        if not info_path.exists(): errors.append(f"{frame_dir.name}: frame_info.json missing"); continue
        info = load_json(info_path)
        actual = {path.name for path in frame_dir.iterdir() if path.is_file()}
        missing = sorted(expected_frame_files(info) - actual)
        if missing: errors.append(f"{frame_dir.name}: missing {missing}")
        for image in info.get("images", []):
            if int(image["payload_type"]) == 2 and not args.skip_pixels:
                path = frame_dir / PAYLOAD_PREFIX[2].format(guid=guid_digits(image["channel_guid"]))
                if path.exists():
                    semantic_images += 1
                    valid, message = validate_semantic_image(path, valid_ids)
                    if not valid: errors.append(f"{frame_dir.name}: {message}")
        scans = info.get("lidar_scans", [])
        for scan in scans:
            suffix = "" if len(scans) == 1 else "_" + guid_digits(scan["sensor_guid"])
            basic, extended = frame_dir / f"lidar{suffix}.bin", frame_dir / f"lidar_extended{suffix}.bin"
            if not basic.exists() or not extended.exists(): continue
            valid, message, points = validate_basic_lidar(basic)
            if not valid: errors.append(f"{frame_dir.name}: {message}"); continue
            valid, message = validate_extended_lidar(extended, points)
            if not valid: errors.append(f"{frame_dir.name}: {message}")
            if points != int(scan.get("hit_count", points)): errors.append(f"{frame_dir.name}: hit_count mismatch")
            if int(scan.get("completed_ray_count", 0)) != int(scan.get("expected_ray_count", 0)): errors.append(f"{frame_dir.name}: incomplete LiDAR revolution")
            total_points += points; lidar_files += 1
    if frame_ids != sorted(set(frame_ids)): errors.append("frame IDs are duplicated or not increasing")
    report = {"session": str(session_dir.resolve()), "frames": len(frame_dirs), "committed_frames": committed, "semantic_ids": sorted(valid_ids), "semantic_images": semantic_images, "lidar_scans": lidar_files, "lidar_points": total_points, "errors": errors, "passed": not errors}
    output = json.dumps(report, ensure_ascii=False, indent=2); print(output)
    if args.report:
        report_path = Path(args.report); report_path.parent.mkdir(parents=True, exist_ok=True); report_path.write_text(output, encoding="utf-8")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
