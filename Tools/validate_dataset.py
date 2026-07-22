#!/usr/bin/env python3
"""
SensorSimulation 数据集校验工具

用法:
    python validate_dataset.py <dataset_session_dir>

功能:
    1. 检查帧目录完整性（RGB + Semantic + LiDAR + GroundTruth）
    2. 验证 Semantic 图只有合法标签值
    3. 验证 BIN 点云文件大小和字段
    4. 验证 Frame ID 连续性
    5. 输出校验报告
"""

import os
import sys
import json
import struct
import argparse
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# 默认合法语义标签集合（与项目中定义一致）
DEFAULT_SEMANTIC_IDS = {0, 1, 2, 3, 4, 5, 6, 7, 8}

def validate_metadata(session_dir: Path) -> Dict:
    """验证并读取 metadata.json"""
    metadata_path = session_dir / "metadata.json"
    if not metadata_path.exists():
        return {"error": "metadata.json not found"}

    with open(metadata_path, 'r') as f:
        return json.load(f)

def find_frame_dirs(session_dir: Path) -> List[Path]:
    """查找所有帧目录"""
    frame_dirs = []
    for item in sorted(session_dir.iterdir()):
        if item.is_dir() and item.name.startswith("frame_"):
            frame_dirs.append(item)
    return frame_dirs

def validate_frame_completeness(frame_dir: Path) -> Dict[str, bool]:
    """检查单帧目录中的文件完整性"""
    return {
        "rgb.png": (frame_dir / "rgb.png").exists(),
        "semantic.png": (frame_dir / "semantic.png").exists(),
        "lidar.bin": (frame_dir / "lidar.bin").exists(),
        "groundtruth.json": (frame_dir / "groundtruth.json").exists(),
        "frame_info.json": (frame_dir / "frame_info.json").exists(),
    }

def validate_semantic_image(png_path: Path, valid_ids: set) -> Tuple[bool, str]:
    """
    验证 Semantic PNG 图像的标签合法性。
    需要安装 Pillow: pip install Pillow
    """
    try:
        from PIL import Image
    except ImportError:
        return True, "Pillow not installed, skipping pixel validation"

    img = Image.open(png_path)
    if img.mode != "RGBA":
        return False, f"Expected RGBA mode, got {img.mode}"

    pixels = img.load()
    width, height = img.size
    invalid_count = 0
    first_invalid = None

    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            if r not in valid_ids or g != 0 or b != 0 or a != 255:
                invalid_count += 1
                if first_invalid is None:
                    first_invalid = (x, y, r, g, b, a)

    if invalid_count > 0:
        return False, f"{invalid_count} invalid pixels, first at {first_invalid}"
    return True, f"All {width*height} pixels valid"

def validate_lidar_bin(bin_path: Path) -> Tuple[bool, str, int]:
    """验证 LiDAR BIN 文件格式"""
    file_size = bin_path.stat().st_size
    if file_size == 0:
        return True, "Empty file (no hits)", 0

    # 每个点 4 个 float32 = 16 字节
    if file_size % 16 != 0:
        return False, f"File size {file_size} not divisible by 16", 0

    point_count = file_size // 16

    # 验证前几个点的数据合理性
    with open(bin_path, 'rb') as f:
        for i in range(min(5, point_count)):
            data = f.read(16)
            x, y, z, intensity = struct.unpack('ffff', data)
            if abs(x) > 1000 or abs(y) > 1000 or abs(z) > 1000:
                return False, f"Point {i}: unreasonable position ({x:.2f}, {y:.2f}, {z:.2f})", point_count
            if not (0.0 <= intensity <= 1.0):
                return False, f"Point {i}: intensity {intensity:.3f} out of [0,1]", point_count

    return True, f"{point_count} points, format OK", point_count

def validate_groundtruth_json(gt_path: Path) -> Tuple[bool, str]:
    """验证 Ground Truth JSON 格式"""
    with open(gt_path, 'r') as f:
        data = json.load(f)

    if "frame_id" not in data:
        return False, "Missing frame_id field"
    if "objects" not in data:
        return False, "Missing objects field"

    for i, obj in enumerate(data["objects"]):
        required_fields = ["instance_id", "semantic_id", "position_x", "position_y", "position_z"]
        for field in required_fields:
            if field not in obj:
                return False, f"Object {i}: missing field '{field}'"

    return True, f"{len(data['objects'])} objects"

def validate_frame_ids(frame_dirs: List[Path]) -> Tuple[bool, str]:
    """验证 Frame ID 连续性"""
    frame_ids = []
    for frame_dir in frame_dirs:
        try:
            frame_id = int(frame_dir.name.replace("frame_", ""))
            frame_ids.append(frame_id)
        except ValueError:
            return False, f"Invalid frame directory name: {frame_dir.name}"

    if not frame_ids:
        return True, "No frames found"

    frame_ids.sort()
    expected = list(range(frame_ids[0], frame_ids[0] + len(frame_ids)))
    if frame_ids != expected:
        missing = set(expected) - set(frame_ids)
        return False, f"Missing frame IDs: {sorted(missing)[:10]}..."

    return True, f"Frame IDs {frame_ids[0]}-{frame_ids[-1]} continuous"

def main():
    parser = argparse.ArgumentParser(description="Validate SensorSimulation dataset")
    parser.add_argument("session_dir", help="Path to dataset session directory")
    parser.add_argument("--semantic-ids", type=str, default=None,
                        help="Comma-separated valid semantic IDs (default: 0-8)")
    parser.add_argument("--skip-pixels", action="store_true",
                        help="Skip pixel-level semantic validation")
    args = parser.parse_args()

    session_dir = Path(args.session_dir)
    if not session_dir.exists():
        print(f"ERROR: Directory not found: {session_dir}")
        sys.exit(1)

    valid_ids = DEFAULT_SEMANTIC_IDS
    if args.semantic_ids:
        valid_ids = set(int(x) for x in args.semantic_ids.split(","))

    print(f"=== SensorSimulation Dataset Validator ===")
    print(f"Session: {session_dir}")
    print(f"Valid semantic IDs: {sorted(valid_ids)}")
    print()

    # 1. 验证 metadata
    print("[1/6] Validating metadata...")
    metadata = validate_metadata(session_dir)
    if "error" in metadata:
        print(f"  WARNING: {metadata['error']}")
    else:
        print(f"  Session ID: {metadata.get('session_id', 'N/A')}")
        print(f"  Mode: {metadata.get('simulation_mode', 'N/A')}")
        print(f"  Seed: {metadata.get('random_seed', 'N/A')}")
    print()

    # 2. 查找帧目录
    print("[2/6] Finding frame directories...")
    frame_dirs = find_frame_dirs(session_dir)
    print(f"  Found {len(frame_dirs)} frame directories")
    print()

    # 3. 验证帧完整性
    print("[3/6] Validating frame completeness...")
    incomplete_frames = []
    for frame_dir in frame_dirs:
        completeness = validate_frame_completeness(frame_dir)
        missing = [k for k, v in completeness.items() if not v]
        if missing:
            incomplete_frames.append((frame_dir.name, missing))

    if incomplete_frames:
        print(f"  WARNING: {len(incomplete_frames)} incomplete frames:")
        for name, missing in incomplete_frames[:5]:
            print(f"    {name}: missing {missing}")
    else:
        print(f"  All {len(frame_dirs)} frames complete")
    print()

    # 4. 验证 Frame ID 连续性
    print("[4/6] Validating frame ID continuity...")
    continuous, msg = validate_frame_ids(frame_dirs)
    print(f"  {'OK' if continuous else 'ERROR'}: {msg}")
    print()

    # 5. 验证 Semantic 图像
    print("[5/6] Validating semantic images...")
    semantic_errors = []
    for frame_dir in frame_dirs:
        semantic_path = frame_dir / "semantic.png"
        if semantic_path.exists() and not args.skip_pixels:
            valid, msg = validate_semantic_image(semantic_path, valid_ids)
            if not valid:
                semantic_errors.append((frame_dir.name, msg))

    if semantic_errors:
        print(f"  ERROR: {len(semantic_errors)} frames with invalid semantics:")
        for name, msg in semantic_errors[:5]:
            print(f"    {name}: {msg}")
    else:
        print(f"  All semantic images valid (or skipped)")
    print()

    # 6. 验证 LiDAR 和 Ground Truth
    print("[6/6] Validating LiDAR and Ground Truth...")
    lidar_errors = []
    gt_errors = []
    total_points = 0

    for frame_dir in frame_dirs:
        lidar_path = frame_dir / "lidar.bin"
        if lidar_path.exists():
            valid, msg, count = validate_lidar_bin(lidar_path)
            total_points += count
            if not valid:
                lidar_errors.append((frame_dir.name, msg))

        gt_path = frame_dir / "groundtruth.json"
        if gt_path.exists():
            valid, msg = validate_groundtruth_json(gt_path)
            if not valid:
                gt_errors.append((frame_dir.name, msg))

    if lidar_errors:
        print(f"  LiDAR errors: {len(lidar_errors)}")
        for name, msg in lidar_errors[:3]:
            print(f"    {name}: {msg}")
    else:
        print(f"  LiDAR: all files valid, {total_points} total points")

    if gt_errors:
        print(f"  Ground Truth errors: {len(gt_errors)}")
        for name, msg in gt_errors[:3]:
            print(f"    {name}: {msg}")
    else:
        print(f"  Ground Truth: all files valid")
    print()

    # 总结
    total_errors = len(incomplete_frames) + len(semantic_errors) + len(lidar_errors) + len(gt_errors)
    if not continuous:
        total_errors += 1

    print("=" * 50)
    if total_errors == 0:
        print("VALIDATION PASSED")
    else:
        print(f"VALIDATION FAILED: {total_errors} issues found")
    print("=" * 50)

    sys.exit(0 if total_errors == 0 else 1)

if __name__ == "__main__":
    main()
