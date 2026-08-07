import json
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image


rgb_path = Path(sys.argv[1])
depth_path = Path(sys.argv[2])
report_path = Path(sys.argv[3])

rgb = np.asarray(Image.open(rgb_path).convert("RGB"), dtype=np.uint8)


def summarize_mask(name, mask):
    ys, xs = np.nonzero(mask)
    if xs.size == 0:
        return {"name": name, "count": 0}
    pixels = rgb[mask]
    return {
        "name": name,
        "count": int(xs.size),
        "centroid_xy": [float(xs.mean()), float(ys.mean())],
        "bbox_xyxy": [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())],
        "mean_rgb": [float(value) for value in pixels.mean(axis=0)],
    }


r, g, b = (rgb[:, :, index].astype(np.int16) for index in range(3))
color_regions = [
    summarize_mask("red", (r > 70) & (r > g + 30) & (r > b + 30)),
    summarize_mask("green", (g > 70) & (g > r + 30) & (g > b + 30)),
    summarize_mask("blue", (b > 70) & (b > r + 30) & (b > g + 30)),
]

os.environ["OPENCV_IO_ENABLE_OPENEXR"] = "1"
import cv2  # noqa: E402

depth = cv2.imread(str(depth_path), cv2.IMREAD_UNCHANGED)
if depth is None:
    raise RuntimeError(f"Failed to read {depth_path}")
if depth.ndim == 3:
    # OpenCV 按 BGRA 返回 EXR；SCS_SceneDepth 写入的逻辑 R 因而位于数组索引 2。
    depth_values = depth[:, :, 2]
else:
    depth_values = depth

finite = depth_values[np.isfinite(depth_values)]
positive = finite[finite > 0.0]
# UE 用很大的 SceneDepth 值表示未命中远背景；验收统计把它与真实几何深度分开。
scene_depth = positive[positive < 1.0e7]
depth_summary = {
    "shape": list(depth.shape),
    "dtype": str(depth.dtype),
    "finite_count": int(finite.size),
    "positive_count": int(positive.size),
    "scene_depth_count": int(scene_depth.size),
}
if scene_depth.size:
    depth_summary.update(
        {
            "min_centimeters": float(scene_depth.min()),
            "max_centimeters": float(scene_depth.max()),
            "percentiles_1_50_99": [
                float(value) for value in np.percentile(scene_depth, [1, 50, 99])
            ],
        }
    )

report = {
    "rgb": {
        "path": str(rgb_path.resolve()),
        "size": [int(rgb.shape[1]), int(rgb.shape[0])],
        "color_regions": color_regions,
        "passed": all(region["count"] > 1000 for region in color_regions),
    },
    "depth_exr": {
        "path": str(depth_path.resolve()),
        **depth_summary,
        "passed": scene_depth.size > 1000 and float(scene_depth.max()) > float(scene_depth.min()),
        "note": "EXR stores the native UE SceneDepth debug target; formal FImagePayload converts centimeters to meters.",
    },
}
report["passed"] = report["rgb"]["passed"] and report["depth_exr"]["passed"]
report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
