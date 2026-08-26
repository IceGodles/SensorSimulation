#!/usr/bin/env python3
"""Create human-readable previews and statistics for one exported sensor frame."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


VIRIDIS_STOPS = np.array(
    [
        [68, 1, 84],
        [59, 82, 139],
        [33, 145, 140],
        [94, 201, 98],
        [253, 231, 37],
    ],
    dtype=np.float32,
)


def colorize(values: np.ndarray) -> np.ndarray:
    scaled = np.clip(values, 0.0, 1.0) * (len(VIRIDIS_STOPS) - 1)
    lower = np.floor(scaled).astype(np.int32)
    upper = np.minimum(lower + 1, len(VIRIDIS_STOPS) - 1)
    fraction = (scaled - lower)[..., None]
    return (VIRIDIS_STOPS[lower] * (1.0 - fraction) + VIRIDIS_STOPS[upper] * fraction).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("session", type=Path)
    parser.add_argument("--frame", default="frame_000001")
    args = parser.parse_args()

    frame = args.session / args.frame
    calibration = json.loads((args.session / "calibration.json").read_text(encoding="utf-8"))
    depth_calibration = next(item for item in calibration["sensors"] if item["payload_type"] == "depth")
    width, height = depth_calibration["image_width"], depth_calibration["image_height"]

    depth_path = next(frame.glob("depth_meters_f32_*.bin"))
    depth = np.fromfile(depth_path, dtype="<f4").reshape(height, width)
    finite = np.isfinite(depth) & (depth > 0.0)
    valid = depth[finite]
    if not valid.size:
        raise RuntimeError("Depth frame contains no finite positive values")

    p01, p05, p50, p95, p99 = np.percentile(valid, [1, 5, 50, 95, 99])
    visualization_far = min(float(p95), 100.0)
    clipped = np.clip(depth, p01, visualization_far)
    normalized = (clipped - p01) / max(visualization_far - p01, 1e-6)
    # Near objects are warm/yellow and far objects are purple.
    preview = colorize(1.0 - normalized)
    preview[~finite] = np.array([0, 0, 0], dtype=np.uint8)
    depth_preview = frame / "depth_preview_0_100m.png"
    Image.fromarray(preview, mode="RGB").save(depth_preview)

    semantic_path = next(frame.glob("semantic_*.png"))
    semantic = np.asarray(Image.open(semantic_path).convert("RGBA"))[:, :, 0]
    semantic_ids, semantic_counts = np.unique(semantic, return_counts=True)
    semantic_preview = np.zeros((height, width, 3), dtype=np.uint8)
    semantic_colors = {
        1: (220, 60, 60),    # road / ground
        5: (70, 130, 230),   # building
        20: (55, 180, 85),   # vegetation
        30: (245, 190, 45),  # street furniture
    }
    for semantic_id, color in semantic_colors.items():
        semantic_preview[semantic == semantic_id] = color
    semantic_preview_path = frame / "semantic_preview.png"
    Image.fromarray(semantic_preview, mode="RGB").save(semantic_preview_path)

    instance_path = next(frame.glob("instance_u32_*.bin"))
    instances = np.fromfile(instance_path, dtype="<u4")
    instance_ids = np.unique(instances)

    stats = {
        "depth": {
            "unit": "meters",
            "valid_pixels": int(valid.size),
            "invalid_pixels": int(depth.size - valid.size),
            "min": float(valid.min()),
            "p01": float(p01),
            "p05": float(p05),
            "median": float(p50),
            "p95": float(p95),
            "p99": float(p99),
            "max": float(valid.max()),
            "preview": str(depth_preview),
        },
        "semantic": {
            "ids": [int(value) for value in semantic_ids],
            "counts": [int(value) for value in semantic_counts],
            "preview": str(semantic_preview_path),
        },
        "instance": {
            "unique_ids": [int(value) for value in instance_ids[:1000]],
            "unique_id_count": int(instance_ids.size),
        },
    }
    stats_path = frame / "visualization_stats.json"
    stats_path.write_text(json.dumps(stats, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(stats, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
